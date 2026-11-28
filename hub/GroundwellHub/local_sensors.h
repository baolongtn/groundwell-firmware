// GROUNDWELL hub v0.12.0: sensors wired straight to the hub, read exactly like a node.
//
// The hub reads its own probes on the node's schedule (every 60 s; 5 s after FAST), builds the reading with the
// node's own packet builder, and hands it to the same code that handles Bluetooth packets. So the dashboard,
// history, cloud upload and diagnostics treat the hub's probes as one more node, shown as "wired".
// Factory calibration works over USB with the node's serial commands and tools/factory_cal.py.
//
// Blocks marked "same as the node" are copied word for word from node/GroundwellNode/GroundwellNode.ino;
// tools/check_shared_code.py (run by the GitHub build) fails the build if they drift apart.
//
// Wiring (XIAO ESP32-C6): the node's wiring, except the photoresistor reads on D2/A2 (only A0..A2 are analog here)
//   Capacitive v2.0 probe  black GND -> GND, red VCC -> D1 (powered only while reading), yellow AOUT -> D0/A0
//   DHT11 module           DATA -> D6, + -> 3V3, - -> GND                                  (optional)
//   GL5539 photoresistor   leg 1 -> D7 (powered only while reading), leg 2 -> D2/A2,
//                          1 kOhm between D2/A2 and GND                                    (optional)
//   I2C sensors            SDA -> D4, SCL -> D5, VIN -> 3V3, GND -> GND: SHT4x or AHT20 (air), LTR390 (light + UV)  (optional)
//                          (no VEML7700 on the hub: it needs a library the hub build doesn't carry; the LTR390 does both jobs)
//   D3, D8, D9 and D10 stay free. Keep Tools > USB CDC On Boot on "Enabled" (the default): with it off, the
//   serial port moves onto D6/D7.
// Which of these are fitted is set on http://plants.local/setup ("Sensors wired to this hub").
#pragma once
#ifndef HUB_I2C_SENSORS
  #define HUB_I2C_SENSORS 1              // 0 leaves the I2C sensors out of the build: about 25 KB less flash
#endif
#include <Arduino.h>
#if HUB_I2C_SENSORS
#include <Wire.h>
#endif
#include <DHT.h>
#include <Preferences.h>
#include <math.h>

// What /setup says is wired (saved as one byte, "hubsens")
enum : uint8_t { LS_ON = 1, LS_DHT = 2, LS_LDR = 4, LS_I2C = 8 };

// ---------------- pins ----------------
static const int PIN_CAP_AOUT  = A0;   // D0
static const int PIN_CAP_POWER = D1;
static const int PIN_LDR_SENSE = A2;   // D2 (on the node it is A5, which can't read analog on the C6)
static const int PIN_DHT       = D6;
static const int PIN_LDR_POWER = D7;
static const char *I2C_PINS    = "D4/D5";

// ---------------- settings (same values as the node) ----------------
static const uint32_t REPORT_MS       = 60000; // one reading every 60 s, like a node (v0.13)
static const uint32_t FAST_REPORT_MS  = 5000;  // after a FAST command, for FAST_FOR_MS
static const uint32_t FAST_FOR_MS     = 300000;
static const float    AIR_JUMP_C      = 3.0f;  // air readings that jump more than this from the last one ...
static const float    AIR_JUMP_RH     = 15.0f;
static const float    AIR_CONFIRM_C   = 1.5f;  // ... are held until the next reading lands within this of them
static const float    AIR_CONFIRM_RH  = 6.0f;
static const uint16_t CAP_SETTLE_MS   = 150;   // capacitive probe power-up settle
static const uint8_t  OVERSAMPLE      = 64;    // the ESP32's ADC is noisier than the nRF52's: average more
static const uint16_t ESP_VDD_MV      = 3300;  // the XIAO's 3.3 V rail: ADC scale and the reported supply
#define DHT_TYPE DHT11                           // DHT22 if you upgrade the sensor
// Photoresistor model: lux = 10 * (LDR_R10_OHMS / R) ^ (1 / LDR_GAMMA). See the node sketch for trimming.
static const float    LDR_FIXED_OHMS  = 1000.0f; // bottom resistor, A2 to GND
static const float    LDR_R10_OHMS    = 75000.0f;
static const float    LDR_GAMMA       = 0.8f;
static const uint16_t LDR_SETTLE_MS   = 5;
// Factory calibration limits for the capacitive probe (0..4095 of the 3.3 V rail), same as the node
static const uint16_t CAL_AIR_MIN   = 2400, CAL_AIR_MAX   = 3900;
static const uint16_t CAL_WATER_MIN = 300,  CAL_WATER_MAX = 2600;
static const uint16_t CAL_SPAN_MIN  = 700;
static const float    CAL_SD_MAX    = 15.0f;
static const uint8_t  CAL_SAMPLES   = 20;
static const uint16_t CAL_GAP_MS    = 500;
static const char    *CAL_FILE      = "/gwcal.bin";

// ---------------- state ----------------
DHT dht(PIN_DHT, DHT_TYPE);
static uint8_t lsMask = 0;                            // LS_* bits from /setup
static bool shtOk = false, ahtOk = false, ltrOk = false;
static const char *airSrc = "none";
static uint16_t ldrAdc = 0;
struct CalData { uint32_t magic; uint16_t air, water, version, check; };
static const uint32_t CAL_MAGIC = 0x31435747;        // "GWC1"
static CalData cal;
static bool calOk = false;
static uint16_t pendAir = 0, pendWater = 0;
static uint32_t fastUntil = 0;
struct AirFilter { bool have, pend; float t, h, pt, ph; };
static Preferences lsPrefs;

// ---------------- measurements ----------------
// One ADC sample as 0..4095 of the 3.3 V rail, the scale the node's math is written for. The ESP32-C6's ADC has
// its own reference, so read calibrated millivolts and scale them (its 12 dB range covers the whole rail).
static uint16_t adcRead(int pin) {
  uint32_t c = ((uint32_t)analogReadMilliVolts(pin) * 4095u + ESP_VDD_MV / 2) / ESP_VDD_MV;
  return (uint16_t)(c > 4095u ? 4095u : c);
}

// Same as the node: adc averaging, capacitive probe
static uint16_t avgRead(int pin, uint8_t n) {
  adcRead(pin);                                      // discard the first sample after switching
  uint32_t acc = 0;
  for (uint8_t i = 0; i < n; i++) acc += adcRead(pin);
  return (uint16_t)(acc / n);
}

// Capacitive probe: power it, let the 555 + filter settle, read, power off.
static uint16_t readCapacitive() {
  digitalWrite(PIN_CAP_POWER, HIGH);
  delay(CAP_SETTLE_MS);
  uint16_t v = avgRead(PIN_CAP_AOUT, OVERSAMPLE);
  digitalWrite(PIN_CAP_POWER, LOW);
  return v;                                          // 0..4095 of VDD (ratiometric)
}

// isnan() is unreliable with fast-math; check the float's exponent bits instead (same as the node).
static bool finite32(float f) {
  uint32_t u;
  memcpy(&u, &f, sizeof u);
  return (u & 0x7F800000u) != 0x7F800000u;
}

// Same as the node: sensor math
// ---- sensor math (host-tested: tools/sensor_math_test.cpp) ----
// Sensirion / Aosong CRC-8: polynomial 0x31, initial value 0xFF.
static uint8_t crc8(const uint8_t *d, uint8_t n) {
  uint8_t c = 0xFF;
  for (uint8_t i = 0; i < n; i++) {
    c ^= d[i];
    for (uint8_t b = 0; b < 8; b++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x31) : (uint8_t)(c << 1);
  }
  return c;
}

// SHT4x frame: T msb, T lsb, crc, RH msb, RH lsb, crc (datasheet section 4.5).
__attribute__((unused)) static bool sht4xConvert(const uint8_t *d, float &tempC, float &rh) {
  if (crc8(d, 2) != d[2] || crc8(d + 3, 2) != d[5]) return false;
  uint16_t st = (uint16_t)((d[0] << 8) | d[1]), srh = (uint16_t)((d[3] << 8) | d[4]);
  tempC = -45.0f + 175.0f * st / 65535.0f;
  rh = -6.0f + 125.0f * srh / 65535.0f;
  if (rh < 0) rh = 0;
  if (rh > 100) rh = 100;
  return true;
}

// AHT20 frame: status, 20-bit RH, 20-bit T, crc (datasheet section 5.4). Bit 7 of status = busy.
__attribute__((unused)) static bool aht20Convert(const uint8_t *d, float &tempC, float &rh) {
  if ((d[0] & 0x80) || crc8(d, 6) != d[6]) return false;
  uint32_t srh = ((uint32_t)d[1] << 12) | ((uint32_t)d[2] << 4) | (d[3] >> 4);
  uint32_t st  = (((uint32_t)d[3] & 0x0F) << 16) | ((uint32_t)d[4] << 8) | d[5];
  rh = srh * 100.0f / 1048576.0f;
  tempC = st * 200.0f / 1048576.0f - 50.0f;
  return true;
}

// LTR390 (datasheet DS86-2015-0004): UV sensitivity 2300 counts per UV index at 18x gain, 20-bit;
// ambient light lux = 0.6 * counts / (gain * integration factor), integration factor 1 at 18-bit (100 ms).
__attribute__((unused)) static const uint8_t LTR_GAIN[5] = {1, 3, 6, 9, 18};
__attribute__((unused)) static float ltrUvIndex(uint32_t counts) { return counts / 2300.0f; }          // at 18x, 20-bit
__attribute__((unused)) static float ltrLux(uint32_t counts, uint8_t gainCode) { return 0.6f * counts / LTR_GAIN[gainCode]; }   // 18-bit

// Air glitch filter: a reading that jumps more than AIR_JUMP_* from the last accepted one is held back;
// it is accepted only if the next reading lands within AIR_CONFIRM_* of it (a real change persists,
// a corrupted DHT11 frame doesn't). Returns false for a held-back reading. (struct AirFilter is with the globals.)
static bool airFilter(AirFilter &f, float t, float h) {
  bool jump = f.have && (fabsf(t - f.t) > AIR_JUMP_C || fabsf(h - f.h) > AIR_JUMP_RH);
  bool confirmed = f.pend && fabsf(t - f.pt) <= AIR_CONFIRM_C && fabsf(h - f.ph) <= AIR_CONFIRM_RH;
  if (!jump || confirmed) { f.have = true; f.pend = false; f.t = t; f.h = h; return true; }
  f.pend = true; f.pt = t; f.ph = h;
  return false;
}
// ---- end sensor math ----

// ---------------- I2C sensors (same as the node) ----------------
#if HUB_I2C_SENSORS
static bool i2cWrite(uint8_t addr, const uint8_t *d, uint8_t n) {
  Wire.beginTransmission(addr);
  Wire.write(d, n);
  return Wire.endTransmission() == 0;
}
static bool i2cRead(uint8_t addr, uint8_t *d, uint8_t n) {
  if (Wire.requestFrom(addr, (size_t)n) != n) return false;
  for (uint8_t i = 0; i < n; i++) d[i] = (uint8_t)Wire.read();
  return true;
}
static bool regWrite(uint8_t addr, uint8_t reg, uint8_t v) { uint8_t d[2] = {reg, v}; return i2cWrite(addr, d, 2); }
static bool regRead(uint8_t addr, uint8_t reg, uint8_t *d, uint8_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;   // repeated start
  return i2cRead(addr, d, n);
}

// Sensirion SHT40/41/45 at 0x44. Idles at about 0.1 uA between readings, no standby command needed.
static const uint8_t SHT4X = 0x44;
static bool sht4xBegin() {
  uint8_t c = 0x94;                                   // soft reset
  if (!i2cWrite(SHT4X, &c, 1)) return false;
  delay(2);
  c = 0x89;                                           // read serial number: proves it is an SHT4x
  if (!i2cWrite(SHT4X, &c, 1)) return false;
  delay(2);
  uint8_t d[6];
  return i2cRead(SHT4X, d, 6) && crc8(d, 2) == d[2] && crc8(d + 3, 2) == d[5];
}
static bool sht4xRead(float &tempC, float &rh) {
  uint8_t c = 0xFD, d[6];                             // high repeatability, heater off
  if (!i2cWrite(SHT4X, &c, 1)) return false;
  delay(10);
  return i2cRead(SHT4X, d, 6) && sht4xConvert(d, tempC, rh);
}

// Aosong AHT20 at 0x38. Sleeps on its own after each measurement.
static const uint8_t AHT20 = 0x38;
static bool aht20Begin() {
  delay(40);                                          // power-on time
  uint8_t st;
  if (!i2cRead(AHT20, &st, 1)) return false;
  if (!(st & 0x08)) {                                 // not calibrated yet: initialise
    uint8_t init[3] = {0xBE, 0x08, 0x00};
    if (!i2cWrite(AHT20, init, 3)) return false;
    delay(10);
  }
  return true;
}
static bool aht20Read(float &tempC, float &rh) {
  uint8_t cmd[3] = {0xAC, 0x33, 0x00}, d[7];
  if (!i2cWrite(AHT20, cmd, 3)) return false;
  delay(80);
  for (uint8_t tries = 0; tries < 4; tries++) {
    if (!i2cRead(AHT20, d, 7)) return false;
    if (!(d[0] & 0x80)) return aht20Convert(d, tempC, rh);
    delay(10);                                        // still busy
  }
  return false;
}

// Lite-On LTR390 at 0x53: UV and ambient light, one measurement at a time, standby (~1 uA) in between.
static const uint8_t LTR390 = 0x53;
static bool ltr390Begin() {
  uint8_t id;
  if (!regRead(LTR390, 0x06, &id, 1) || (id >> 4) != 0x0B) return false;   // PART_ID 0xB2
  return regWrite(LTR390, 0x00, 0x00);                // standby
}
// uv: UVS mode, else ambient light. resCode 0 = 20-bit (400 ms), 2 = 18-bit (100 ms).
static bool ltr390Measure(bool uv, uint8_t gainCode, uint8_t resCode, uint32_t &counts) {
  uint8_t rateCode = resCode == 0 ? 4 : 2;            // 500 ms or 100 ms period, at least the conversion time
  uint16_t convMs = resCode == 0 ? 400 : 100;
  uint8_t st, d[3];
  if (!regWrite(LTR390, 0x04, (uint8_t)((resCode << 4) | rateCode)) || !regWrite(LTR390, 0x05, gainCode)) return false;
  regRead(LTR390, 0x07, &st, 1);                      // clear any old data-ready flag
  if (!regWrite(LTR390, 0x00, (uint8_t)(0x02 | (uv ? 0x08 : 0x00)))) return false;   // enable, mode
  delay(convMs + 20);
  bool ready = false;
  for (uint8_t i = 0; i < 10 && !ready; i++) {
    ready = regRead(LTR390, 0x07, &st, 1) && (st & 0x08);
    if (!ready) delay(20);
  }
  bool ok = ready && regRead(LTR390, uv ? 0x10 : 0x0D, d, 3);
  regWrite(LTR390, 0x00, 0x00);                       // back to standby
  if (!ok) return false;
  counts = (uint32_t)d[0] | ((uint32_t)d[1] << 8) | (((uint32_t)d[2] & 0x0F) << 16);
  return true;
}
static bool ltr390Lux(float &lux) {
  uint32_t c;
  if (!ltr390Measure(false, 1, 2, c)) return false;  // 3x, 18-bit: 0.2 lux steps up to ~52,000 lux
  if (c >= 250000) {                                  // near full scale: retry at 1x (up to ~157,000 lux)
    if (!ltr390Measure(false, 0, 2, c)) return false;
    lux = ltrLux(c, 0);
  } else lux = ltrLux(c, 1);
  return true;
}
static bool readUV(float &uvi) {
  uint32_t c;
  if (!ltrOk || !ltr390Measure(true, 4, 0, c)) return false;   // 18x, 20-bit: the datasheet's UV calibration point
  uvi = ltrUvIndex(c);
  if (uvi > 25.5f) uvi = 25.5f;                       // BTHome u8 x0.1
  return true;
}
#else
static bool sht4xRead(float &, float &) { return false; }
static bool aht20Read(float &, float &) { return false; }
static bool ltr390Lux(float &) { return false; }
static bool readUV(float &) { return false; }
#endif
// Same as the node: DHT11
static bool readDHT(float &tempC, float &rh) {
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (!finite32(h) || !finite32(t)) {
    delay(50);                                       // one retry: a radio event can disturb the 4 ms bit stream
    h = dht.readHumidity(true);
    t = dht.readTemperature(false, true);
  }
  if (!finite32(h) || !finite32(t) || !(h >= 0 && h <= 100) || !(t >= -40 && t <= 80)) return false;
  tempC = t;
  rh = h;
  return true;
}

// Best temperature/humidity source found at boot.
static bool readAir(float &tempC, float &rh) {
  if (shtOk) return sht4xRead(tempC, rh);
  if (ahtOk) return aht20Read(tempC, rh);
  if (lsMask & LS_DHT) return readDHT(tempC, rh);
  return false;
}
static bool hasAirSensor() { return shtOk || ahtOk || (lsMask & LS_DHT); }

// Same as the node: photoresistor conversion
__attribute__((unused)) static float ldrLux(uint16_t adc) {
  if (adc < 1) return 0.0f;                          // darker than about 0.1 lux, or the LDR is unplugged
  if (adc > 4094) adc = 4094;                        // saturated: brighter than the 1k divider can resolve
  float r = LDR_FIXED_OHMS * (4095.0f - adc) / adc;  // LDR resistance, ohms
  return 10.0f * powf(LDR_R10_OHMS / r, 1.0f / LDR_GAMMA);
}

static const char *lightName() { return ltrOk ? "ltr390" : (lsMask & LS_LDR) ? "ldr" : "none"; }

// The LTR390 if one was found, else the photoresistor: power the divider, let the input settle, read, power off.
static bool readLight(float &lux) {
  float v;
  if (ltrOk) {
    if (!ltr390Lux(v) || !finite32(v) || v < 0 || v > 140000) return false;
    lux = v;
    return true;
  }
  if (!(lsMask & LS_LDR)) return false;
  digitalWrite(PIN_LDR_POWER, HIGH);
  delay(LDR_SETTLE_MS);
  ldrAdc = avgRead(PIN_LDR_SENSE, OVERSAMPLE);
  digitalWrite(PIN_LDR_POWER, LOW);
  v = ldrLux(ldrAdc);
  if (!finite32(v)) return false;
  lux = v > 140000.0f ? 140000.0f : v;
  return true;
}

// ---------------- calibration storage: the hub's settings memory (namespace "gwlocal") ----------------
static const char *prefKey(const char *path) { return path[0] == '/' ? path + 1 : path; }
static bool storeRead(const char *path, void *buf, size_t n) {
  return lsPrefs.isKey(prefKey(path)) && lsPrefs.getBytes(prefKey(path), buf, n) == n;
}
static bool storeWrite(const char *path, const void *buf, size_t n) { return lsPrefs.putBytes(prefKey(path), buf, n) == n; }
static void storeRemove(const char *path) { if (lsPrefs.isKey(prefKey(path))) lsPrefs.remove(prefKey(path)); }

// ---------------- factory calibration (same as the node) ----------------
static uint16_t calCheck(const CalData &c) { return (uint16_t)(c.air ^ c.water ^ c.version ^ 0xA5A5u); }

static void loadCal() {
  calOk = false;
  CalData c;
  if (storeRead(CAL_FILE, &c, sizeof c) && c.magic == CAL_MAGIC && c.check == calCheck(c) && c.air > c.water) { cal = c; calOk = true; }
}

static bool saveCal(uint16_t air, uint16_t water) {
  CalData c = {CAL_MAGIC, air, water, 1, 0};
  c.check = calCheck(c);
  bool ok = storeWrite(CAL_FILE, &c, sizeof c);
  loadCal();
  return ok && calOk && cal.air == air && cal.water == water;
}

// Probe scale: 0% = the unit's own air reading, 100% = its own water reading, as hundredths.
static uint16_t probeScaleX100(uint16_t raw) {
  long span = (long)cal.air - (long)cal.water;
  long v = ((long)cal.air - (long)raw) * 10000L / span;
  return (uint16_t)(v < 0 ? 0 : v > 10000 ? 10000 : v);
}

// Returns a reason string if the pair fails, or nullptr if it passes.
static const char *calReject(uint16_t air, uint16_t water) {
  if (air < CAL_AIR_MIN || air > CAL_AIR_MAX) return "air_out_of_range";
  if (water < CAL_WATER_MIN || water > CAL_WATER_MAX) return "water_out_of_range";
  if (air < water + CAL_SPAN_MIN) return "span_too_small";
  return nullptr;
}

static uint16_t readCapacitive();
static bool measurePoint(const char *label, uint16_t &out) {
  uint16_t v[CAL_SAMPLES];                           // two passes in double: one-pass float variance
  double sum = 0, ss = 0;                            // loses about 1 count of precision at 3000
  for (uint8_t i = 0; i < CAL_SAMPLES; i++) {
    v[i] = readCapacitive();
    sum += v[i];
    delay(CAL_GAP_MS > CAP_SETTLE_MS ? CAL_GAP_MS - CAP_SETTLE_MS : 0);
  }
  double mean = sum / CAL_SAMPLES;
  for (uint8_t i = 0; i < CAL_SAMPLES; i++) ss += (v[i] - mean) * (v[i] - mean);
  float sd = (float)sqrt(ss / CAL_SAMPLES);
  out = (uint16_t)lround(mean);
  bool steady = sd <= CAL_SD_MAX;
  Serial.printf("CAL %s mean=%.1f sd=%.1f n=%u %s\n", label, (float)mean, sd, CAL_SAMPLES, steady ? "OK" : "FAIL reason=unstable");
  return steady;
}

static void finishCal(uint16_t air, uint16_t water) {
  const char *why = calReject(air, water);
  if (why) { Serial.printf("CAL RESULT FAIL reason=%s air=%u water=%u\n", why, air, water); return; }
  if (!saveCal(air, water)) { Serial.println("CAL RESULT FAIL reason=flash_write"); return; }
  Serial.printf("CAL RESULT PASS air=%u water=%u span=%u saved\n", air, water, air - water);
}

// Same as the node: BTHome v2 packet
static uint8_t buildBTHome(uint8_t *b, uint8_t pid, bool airOk, float tempC, float rh,
                           bool luxOk, float lux, float vdd, int32_t capPct, uint16_t cap,
                           bool uvOk, float uvi) {
  uint8_t n = 0;
  uint16_t mv = (uint16_t)lroundf(vdd * 1000.0f);
  b[n++] = 0xD2; b[n++] = 0xFC;                      // UUID 0xFCD2
  b[n++] = 0x40;                                     // BTHome v2, unencrypted, regular interval
  b[n++] = 0x00; b[n++] = pid;
  if (airOk) {
    int16_t t   = (int16_t)lroundf(tempC * 100.0f);
    uint16_t h  = (uint16_t)lroundf(rh * 100.0f);
    b[n++] = 0x02; b[n++] = (uint8_t)(t & 0xFF); b[n++] = (uint8_t)((uint16_t)t >> 8);
    b[n++] = 0x03; b[n++] = (uint8_t)(h & 0xFF); b[n++] = (uint8_t)(h >> 8);
  }
  if (luxOk) {
    float lx = lux * 100.0f;
    uint32_t l = lx >= 16777215.0f ? 0xFFFFFFu : (uint32_t)lroundf(lx);   // u24 tops out at 167,772 lux
    b[n++] = 0x05; b[n++] = (uint8_t)(l & 0xFF); b[n++] = (uint8_t)(l >> 8); b[n++] = (uint8_t)(l >> 16);
  }
  b[n++] = 0x0C; b[n++] = (uint8_t)(mv & 0xFF);  b[n++] = (uint8_t)(mv >> 8);
  if (capPct >= 0) {
    uint16_t m = (uint16_t)capPct;
    b[n++] = 0x14; b[n++] = (uint8_t)(m & 0xFF); b[n++] = (uint8_t)(m >> 8);
  } else {
    b[n++] = 0x3D; b[n++] = (uint8_t)(cap & 0xFF); b[n++] = (uint8_t)(cap >> 8);
  }
  if (uvOk) {
    float u = uvi * 10.0f;
    b[n++] = 0x46; b[n++] = (uint8_t)(u <= 0 ? 0 : u >= 255.0f ? 255 : lroundf(u));
  }
  return n;                                          // 23 bytes with air + light + UV (the legacy advert allows 26), 11 with none
}
// ---- end BTHome packet ----

static bool localFast() { return fastUntil && (int32_t)(fastUntil - millis()) > 0; }   // FAST: 5 s readings for 5 minutes

// ---------------- start ----------------
static void lsBegin(uint8_t mask) {
  lsMask = mask;
  lsPrefs.begin("gwlocal", false);
  loadCal();
  if (!(mask & LS_ON)) return;
  pinMode(PIN_CAP_POWER, OUTPUT);
  digitalWrite(PIN_CAP_POWER, LOW);
  pinMode(PIN_LDR_POWER, OUTPUT);
  digitalWrite(PIN_LDR_POWER, LOW);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);                    // 12 dB: 0 to 3.3 V on the ESP32-C6
  if (mask & LS_DHT) dht.begin();
#if HUB_I2C_SENSORS
  if (mask & LS_I2C) {
    Wire.begin();                                    // SDA D4, SCL D5
    Wire.setTimeOut(50);
    shtOk  = sht4xBegin();
    ahtOk  = !shtOk && aht20Begin();
    ltrOk  = ltr390Begin();
  }
#endif
  airSrc = shtOk ? "sht4x" : ahtOk ? "aht20" : (mask & LS_DHT) ? "dht11" : "none";
}

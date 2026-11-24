/*
  GROUNDWELL sensor node v0.9.0  (two soil probes + air temperature/humidity + light + UV, one board)
  Board:  Seeed XIAO nRF52840 (plain or Sense)
          Arduino IDE: Boards Manager URL
            https://files.seeedstudio.com/arduino/package_seeeduino_boards_index.json
          install "Seeed nRF52 Boards" (NOT the "mbed-enabled" one), select "Seeed XIAO nRF52840"
          The Bluefruit BLE library ships with that core.
          Library Manager: install "DHT sensor library" by Adafruit (accept its "Adafruit Unified Sensor" dependency).
          Only if you set LIGHT_SENSOR to LIGHT_I2C: also install "Adafruit VEML7700 Library"
          (accept its "Adafruit BusIO" dependency). The SHT4x, AHT20 and LTR390 need no library.

  What it does
    Every REPORT_MS (30 s) it reads both probes, air temperature/humidity and light, then broadcasts one
    BTHome v2 packet for ADV_BURST_S (6 s) and lets the radio sleep until the next reading:
      packet id, air temperature + humidity (best sensor found; skipped if a read fails or looks like a glitch),
      illuminance in lux, supply voltage (the hub and cloud derive battery level from it),
      capacitive probe: factory-calibrated moisture (BTHome moisture %, 0 = air, 100 = water)
                        or, before factory calibration, its raw count (0..4095, LOWER = wetter)
      rod probe:        raw count (0..4095, HIGHER = wetter)
      UV index:         only with an LTR390 fitted
    Low power, on USB or battery alike: the CPU sleeps between readings, the capacitive probe, photoresistor and
    rod drive are powered only while reading, the I2C sensors sit in standby between readings, and the radio
    is off except for the 6 s burst after each reading.

  Air and light sensors (LIGHT_I2C wiring): found automatically at boot, best first
      temperature/humidity  Sensirion SHT40/41/45 (0x44) -> AHT20 (0x38) -> DHT11 on D6
      light                 VEML7700 (0x10) -> LTR390 ambient light (0x53)
      UV index              LTR390 (0x53)
    With the photoresistor wiring (LIGHT_LDR) there is no I2C bus: DHT11 + GL5539 only.
    A reading that jumps more than 3 C or 15 %RH from the last one is held back until the next reading
    confirms it, so a single corrupted DHT11 frame never reaches the dashboard.

  Factory calibration (v0.6), over USB serial at 115200, one command per line:
      INFO                 firmware, address, calibration
      CAL AIR              probe clean, dry, in open air: averages 20 readings over 10 s
      CAL WATER            probe in distilled water up to its max line: same
                           After both, the node checks the limits below and saves to its own flash.
      CAL SET <air> <water>  enter the two points by hand (same limit checks)
      CAL CLEAR            erase the calibration (the node goes back to sending raw counts)
      FAST                 readings every 5 s for the next 5 minutes (factory checks and bench tests)
    Every reply starts with INFO or CAL and ends in OK, PASS or FAIL, so tools/factory_cal.py can drive it.
    The calibration lives on the node, so a node ships calibrated with no plant and no hub settings.

  Wiring choice: pick one with LIGHT_SENSOR in the settings below.
    LIGHT_LDR (default)  GL5539 photoresistor on D7 + A5, rod reference on D4, DHT11 on D6
    LIGHT_I2C            I2C bus on D4 (SDA) / D5 (SCL) for any mix of SHT4x, AHT20, VEML7700, LTR390;
                         rod reference moves to D7; the photoresistor is not used (the LTR390 or VEML7700
                         measures light); the DHT11 on D6 is still used if no SHT4x/AHT20 is found.
  The packet, hub and cloud are identical either way.

  Wiring
    Capacitive v2.0 probe (3-wire cable)
      black  GND   -> XIAO GND
      red    VCC   -> XIAO D1    (powered only while reading)
      yellow AOUT  -> XIAO D0/A0
    Rod probe (2-wire cable; the HD-38 comparator board is NOT used)
      rod wire 1   -> XIAO D3                     (drive A)
      rod wire 2   -> XIAO D2/A2                  (sense)
      10 kOhm      -> between XIAO D2/A2 and D4   (reference, drive B)   LIGHT_LDR
                      between XIAO D2/A2 and D7                          LIGHT_I2C     
      Rod polarity doesn't matter.
    DHT11 module (3 pins, labels vary: S/DATA, +/VCC, -/GND)
      S / DATA     -> XIAO D6                                            (was D5 before v0.4)
      + / VCC      -> XIAO 3V3
      - / GND      -> XIAO GND
      Bare 4-pin DHT11 (no module board): add a 10 kOhm pull-up from DATA to 3V3.
    GL5539 photoresistor (LIGHT_LDR, default)
      leg 1        -> XIAO D7    (powered only for a few ms per reading)
      leg 2        -> XIAO D5/A5
      1 kOhm       -> between XIAO D5/A5 and GND   (set LDR_FIXED_OHMS to 10000 if you fit a 10k)
      Either leg; polarity doesn't matter. Face it up at leaf height.
    I2C breakouts (LIGHT_I2C): SHT40 (Adafruit 4885), AHT20 (4566), VEML7700 (4162), LTR390 (4831), any mix.
      All of them share the same four wires (daisy-chain STEMMA QT boards, or wire them in parallel):
      SDA          -> XIAO D4    (the XIAO's I2C pins, which is why the rod reference moves to D7)
      SCL          -> XIAO D5
      VIN / VCC    -> XIAO 3V3
      GND          -> XIAO GND
      Light sensors: face up with a clear view of the window. Temperature/humidity: in open air at the plant,
      away from the board and out of direct sun. Anything not found at boot is simply left out of the packet.
    Power: USB-C or a battery; the firmware runs the same low-power cycle on both. If you fit a coin cell to
    the BAT pads, remove it before plugging USB in (the onboard charger would try to charge it).

  Why the rods are driven this way
    The current through the soil is reversed on every reading and both phases
    last the same time, so there is no net DC: the rods don't electroplate or
    corrode, and soil polarization cancels out.
*/

static const char *FW_VERSION = "0.9.0";

// ---------------- light sensor choice ----------------
#define LIGHT_LDR      1                         // GL5539 photoresistor + DHT11, no I2C (the parts in hand)
#define LIGHT_I2C      2                         // I2C sensors on D4/D5: SHT4x / AHT20 / VEML7700 / LTR390
#define LIGHT_VEML7700 LIGHT_I2C                 // older name for the same wiring
#define LIGHT_SENSOR   LIGHT_LDR                 // <-- change to LIGHT_I2C when you fit I2C sensors

#include <bluefruit.h>
#include <DHT.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
using namespace Adafruit_LittleFS_Namespace;
#if LIGHT_SENSOR == LIGHT_I2C
#include <Wire.h>
#include <Adafruit_VEML7700.h>
#endif

// ---------------- settings ----------------
static const uint32_t REPORT_MS       = 30000; // new reading every 30 s (soil changes over hours)
static const uint32_t FAST_REPORT_MS  = 5000;  // after a FAST command, for FAST_FOR_MS
static const uint32_t FAST_FOR_MS     = 300000;
static const uint16_t ADV_INTERVAL_MS = 250;   // within a burst, repeat the packet every 250 ms ...
static const uint8_t  ADV_BURST_S     = 6;     // ... for 6 s (~24 copies), then the radio sleeps
static const float    AIR_JUMP_C      = 3.0f;  // air readings that jump more than this from the last one ...
static const float    AIR_JUMP_RH     = 15.0f;
static const float    AIR_CONFIRM_C   = 1.5f;  // ... are held until the next reading lands within this of them
static const float    AIR_CONFIRM_RH  = 6.0f;
static const int8_t   TX_POWER_DBM    = 4;     // 0 or 4; raise to 8 if the hub is far away
static const uint16_t CAP_SETTLE_MS   = 150;   // capacitive probe power-up settle
static const uint8_t  OVERSAMPLE      = 16;
#define DHT_TYPE DHT11                           // DHT22 if you upgrade the sensor

// Photoresistor model (LIGHT_LDR only). lux = 10 * (LDR_R10_OHMS / R) ^ (1 / LDR_GAMMA)
// GL5539 datasheet: 50 to 100 kOhm at 10 lux, gamma about 0.8. Parts vary by up to about +-40%:
// to trim a node, hold a phone light-meter app beside it in steady light and adjust LDR_R10_OHMS
// (higher R10 -> higher lux) until the two agree.
static const float    LDR_FIXED_OHMS  = 1000.0f; // bottom resistor, A5 to GND (1k spreads 10..100,000 lux)
static const float    LDR_R10_OHMS    = 75000.0f;
static const float    LDR_GAMMA       = 0.8f;
static const uint16_t LDR_SETTLE_MS   = 5;

// Factory calibration limits for the capacitive probe (raw counts, 12-bit, ratiometric to VDD).
// A unit outside these fails calibration: wrong timer chip, damaged coating, probe not in water, and so on.
static const uint16_t CAL_AIR_MIN   = 2400, CAL_AIR_MAX   = 3900;
static const uint16_t CAL_WATER_MIN = 300,  CAL_WATER_MAX = 2600;
static const uint16_t CAL_SPAN_MIN  = 700;   // air minus water
static const float    CAL_SD_MAX    = 15.0f; // counts; a probe still moving or settling fails
static const uint8_t  CAL_SAMPLES   = 20;    // 20 readings ...
static const uint16_t CAL_GAP_MS    = 500;   // ... 0.5 s apart = 10 s
static const char    *CAL_FILE      = "/gwcal.bin";

// ---------------- pins ----------------
static const int PIN_CAP_AOUT  = A0;   // D0
static const int PIN_CAP_POWER = D1;
static const int PIN_ROD_SENSE = A2;   // D2
static const int PIN_ROD_A     = D3;
static const int PIN_DHT       = D6;
#if LIGHT_SENSOR == LIGHT_I2C
static const int PIN_ROD_REF   = D7;   // through the 10k to D2 (D4/D5 are the I2C pins)
#else
static const int PIN_ROD_REF   = D4;   // through the 10k to D2
static const int PIN_LDR_POWER = D7;
static const int PIN_LDR_SENSE = A5;   // D5
#endif

DHT dht(PIN_DHT, DHT_TYPE);
#if LIGHT_SENSOR == LIGHT_I2C
Adafruit_VEML7700 veml;
static bool vemlOk = false, shtOk = false, ahtOk = false, ltrOk = false;
#endif
static const char *airSrc = "dht11";
__attribute__((unused)) static uint16_t ldrAdc = 0;           // last raw photoresistor reading, for the serial log

static uint8_t packetId = 0;

struct CalData { uint32_t magic; uint16_t air, water, version, check; };
static const uint32_t CAL_MAGIC = 0x31435747;        // "GWC1"
static CalData cal;
static bool calOk = false;
static uint16_t pendAir = 0, pendWater = 0;           // this session's CAL AIR / CAL WATER results
static uint32_t lastSentAt = 0;                       // for the dt= cadence check in the serial log
static uint32_t fastUntil = 0;                        // FAST command: 5 s readings until this millis()
// Declared here, before any function: the Arduino IDE puts a prototype of every function above the first one.
struct AirFilter { bool have, pend; float t, h, pt, ph; };

// ---------------- measurements ----------------
static uint16_t avgRead(int pin, uint8_t n) {
  analogRead(pin);                                   // discard the first sample after switching
  uint32_t acc = 0;
  for (uint8_t i = 0; i < n; i++) acc += analogRead(pin);
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

// Rod probe: two equal phases with opposite polarity.
//   phase 1: A=HIGH, REF=LOW  -> v1 = Rref/(Rsoil+Rref)
//   phase 2: A=LOW,  REF=HIGH -> v2 = Rsoil/(Rsoil+Rref)
// wetness = average of v1 and (1 - v2); 0 = open circuit (dry), 4095 = short (wet).
static uint16_t readRods() {
  pinMode(PIN_ROD_A, OUTPUT);
  pinMode(PIN_ROD_REF, OUTPUT);

  digitalWrite(PIN_ROD_A, HIGH);
  digitalWrite(PIN_ROD_REF, LOW);
  delayMicroseconds(300);
  uint16_t v1 = avgRead(PIN_ROD_SENSE, 8);

  digitalWrite(PIN_ROD_A, LOW);
  digitalWrite(PIN_ROD_REF, HIGH);
  delayMicroseconds(300);
  uint16_t v2 = avgRead(PIN_ROD_SENSE, 8);

  pinMode(PIN_ROD_A, INPUT);                         // both high-Z: no current between readings
  pinMode(PIN_ROD_REF, INPUT);
  return (uint16_t)(((uint32_t)v1 + (4095u - v2)) / 2u);
}

// Supply voltage through the SAADC's internal VDD channel (no divider, no leakage).
static float readVDD() {
  volatile int16_t result = 0;
  NRF_SAADC->RESOLUTION = SAADC_RESOLUTION_VAL_12bit;
  NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Enabled;
  // analogRead() leaves channel 0 connected to the last pin it read. The SAADC samples every
  // connected channel in order, so with one result slot we would get that pin, not VDD.
  for (int i = 0; i < 7; i++) {
    NRF_SAADC->CH[i].PSELP = SAADC_CH_PSELP_PSELP_NC;
    NRF_SAADC->CH[i].PSELN = SAADC_CH_PSELN_PSELN_NC;
  }
  NRF_SAADC->CH[7].PSELN = SAADC_CH_PSELN_PSELN_NC;
  NRF_SAADC->CH[7].PSELP = SAADC_CH_PSELP_PSELP_VDD;
  NRF_SAADC->CH[7].CONFIG = (SAADC_CH_CONFIG_GAIN_Gain1_6 << SAADC_CH_CONFIG_GAIN_Pos) |
                            (SAADC_CH_CONFIG_REFSEL_Internal << SAADC_CH_CONFIG_REFSEL_Pos) |
                            (SAADC_CH_CONFIG_TACQ_10us << SAADC_CH_CONFIG_TACQ_Pos);
  NRF_SAADC->RESULT.PTR = (uint32_t)&result;
  NRF_SAADC->RESULT.MAXCNT = 1;
  NRF_SAADC->TASKS_START = 1;
  while (!NRF_SAADC->EVENTS_STARTED) {}
  NRF_SAADC->EVENTS_STARTED = 0;
  NRF_SAADC->TASKS_SAMPLE = 1;
  while (!NRF_SAADC->EVENTS_END) {}
  NRF_SAADC->EVENTS_END = 0;
  NRF_SAADC->TASKS_STOP = 1;
  while (!NRF_SAADC->EVENTS_STOPPED) {}
  NRF_SAADC->EVENTS_STOPPED = 0;
  NRF_SAADC->CH[7].PSELP = SAADC_CH_PSELP_PSELP_NC;
  NRF_SAADC->ENABLE = SAADC_ENABLE_ENABLE_Disabled;
  if (result < 0) result = 0;
  return result * 3.6f / 4096.0f;                    // gain 1/6, 0.6 V reference
}

// DHT11: 1 s minimum between reads (we read every 5 s). A failed read returns false and
// the packet simply omits temperature/humidity that cycle; the hub keeps the last values.
// isnan() is unreliable here: the core builds with fast-math, which lets the compiler assume
// NaN never happens. Check the float's exponent bits directly instead.
static bool finite32(float f) {
  uint32_t u;
  memcpy(&u, &f, sizeof u);
  return (u & 0x7F800000u) != 0x7F800000u;
}


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

#if LIGHT_SENSOR == LIGHT_I2C
// ---------------- I2C sensors ----------------
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
static bool readUV(float &) { return false; }
#endif

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
#if LIGHT_SENSOR == LIGHT_I2C
  if (shtOk) return sht4xRead(tempC, rh);
  if (ahtOk) return aht20Read(tempC, rh);
#endif
  return readDHT(tempC, rh);
}

// Photoresistor on a divider: D7 (VDD) -> LDR -> A5 -> LDR_FIXED_OHMS -> GND.
// The ADC is referenced to VDD, so adc/4095 = Rf / (R + Rf) whatever the supply voltage.
__attribute__((unused)) static float ldrLux(uint16_t adc) {
  if (adc < 1) return 0.0f;                          // darker than about 0.1 lux, or the LDR is unplugged
  if (adc > 4094) adc = 4094;                        // saturated: brighter than the 1k divider can resolve
  float r = LDR_FIXED_OHMS * (4095.0f - adc) / adc;  // LDR resistance, ohms
  return 10.0f * powf(LDR_R10_OHMS / r, 1.0f / LDR_GAMMA);
}

#if LIGHT_SENSOR == LIGHT_I2C
// VEML7700 with automatic gain and integration time, so it covers a dim corner (a few lux)
// through direct sun (~100,000 lux) without saturating; shut down (0.5 uA) between readings.
// Falls back to the LTR390's ambient light sensor. Returns false if neither is fitted.
static bool readLight(float &lux) {
  float v;
  if (vemlOk) {
    veml.enable(true);
    delay(5);
    v = veml.readLux(VEML_LUX_AUTO);
    veml.enable(false);
  } else if (!ltrOk || !ltr390Lux(v)) return false;
  if (!finite32(v) || v < 0 || v > 140000) return false;
  lux = v;
  return true;
}
#else
// GL5539: power the divider, let the ADC input settle, read, power off.
static bool readLight(float &lux) {
  digitalWrite(PIN_LDR_POWER, HIGH);
  delay(LDR_SETTLE_MS);
  ldrAdc = avgRead(PIN_LDR_SENSE, OVERSAMPLE);
  digitalWrite(PIN_LDR_POWER, LOW);
  float v = ldrLux(ldrAdc);
  if (!finite32(v)) return false;
  lux = v > 140000.0f ? 140000.0f : v;
  return true;
}
#endif

// ---------------- factory calibration ----------------
static uint16_t calCheck(const CalData &c) { return (uint16_t)(c.air ^ c.water ^ c.version ^ 0xA5A5u); }

static void loadCal() {
  calOk = false;
  File f(InternalFS);
  if (!f.open(CAL_FILE, FILE_O_READ)) return;
  CalData c;
  bool ok = f.read(&c, sizeof c) == (int)sizeof c;
  f.close();
  if (ok && c.magic == CAL_MAGIC && c.check == calCheck(c) && c.air > c.water) { cal = c; calOk = true; }
}

static bool saveCal(uint16_t air, uint16_t water) {
  CalData c = {CAL_MAGIC, air, water, 1, 0};
  c.check = calCheck(c);
  InternalFS.remove(CAL_FILE);                       // FILE_O_WRITE appends, so start fresh
  File f(InternalFS);
  if (!f.open(CAL_FILE, FILE_O_WRITE)) return false;
  bool ok = f.write((const uint8_t *)&c, sizeof c) == sizeof c;
  f.close();
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

static const char *lightName() {
#if LIGHT_SENSOR == LIGHT_I2C
  return vemlOk ? "veml7700" : ltrOk ? "ltr390" : "none";
#else
  return "ldr";
#endif
}

static void printInfo() {
  ble_gap_addr_t a = Bluefruit.getAddr();
  Serial.printf("INFO fw=%s mac=%02X:%02X:%02X:%02X:%02X:%02X interval_ms=%lu light=%s ",
                FW_VERSION, a.addr[5], a.addr[4], a.addr[3], a.addr[2], a.addr[1], a.addr[0],
                (unsigned long)REPORT_MS, lightName());
#if LIGHT_SENSOR == LIGHT_I2C
  Serial.printf("air=%s uv=%s ", airSrc, ltrOk ? "ltr390" : "none");
#else
  Serial.printf("air=%s uv=none ", airSrc);
#endif
  if (calOk) Serial.printf("cal=yes air=%u water=%u OK\n", cal.air, cal.water);
  else       Serial.println("cal=no OK");
}

static void handleCommand(char *line) {
  for (char *c = line; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;   // case-insensitive
  unsigned a = 0, w = 0;
  if (!strcmp(line, "INFO")) printInfo();
  else if (!strcmp(line, "CAL AIR"))   { if (measurePoint("AIR", pendAir))     { if (pendWater) finishCal(pendAir, pendWater); } else pendAir = 0; }
  else if (!strcmp(line, "CAL WATER")) { if (measurePoint("WATER", pendWater)) { if (pendAir) finishCal(pendAir, pendWater); } else pendWater = 0; }
  else if (sscanf(line, "CAL SET %u %u", &a, &w) == 2) finishCal((uint16_t)a, (uint16_t)w);
  else if (!strcmp(line, "CAL CLEAR")) { InternalFS.remove(CAL_FILE); calOk = false; pendAir = pendWater = 0; Serial.println("CAL CLEARED OK"); }
  else if (!strcmp(line, "FAST"))      { fastUntil = millis() + FAST_FOR_MS; Serial.printf("FAST interval_ms=%lu for_s=%lu OK\n", (unsigned long)FAST_REPORT_MS, (unsigned long)(FAST_FOR_MS / 1000)); }
  else if (line[0]) Serial.printf("ERR unknown command: %s\n", line);
}

static void serviceSerial() {
  static char buf[48];
  static uint8_t n = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') { buf[n] = 0; if (n) handleCommand(buf); n = 0; }
    else if (n < sizeof buf - 1) buf[n++] = c;
  }
}

// ---------------- BTHome v2 packet ----------------
// Service data layout (all little-endian), objects in ascending ID order:
//   D2 FC | 40 | 00 pid | [02 temp*100 (s16) | 03 RH*100 (u16)] | [05 lux*100 (u24)]
//         | 0C mV (u16) | [14 moisture*100 (u16), calibrated capacitive] | [3D cap raw (u16), uncalibrated] | 3D rods (u16)
//         | [46 UV index*10 (u8)]
// capPct is the probe scale x100 (0..10000) when calibrated, or -1 to send the raw capacitive count instead.
// Battery % (0x01) is no longer sent (v0.9): the supply voltage carries the same information, and the
// two bytes make room for the UV index inside the 31-byte legacy advert.
static uint8_t buildBTHome(uint8_t *b, uint8_t pid, bool airOk, float tempC, float rh,
                           bool luxOk, float lux, float vdd, int32_t capPct, uint16_t cap, uint16_t rods,
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
  b[n++] = 0x3D; b[n++] = (uint8_t)(rods & 0xFF);b[n++] = (uint8_t)(rods >> 8);
  if (uvOk) {
    float u = uvi * 10.0f;
    b[n++] = 0x46; b[n++] = (uint8_t)(u <= 0 ? 0 : u >= 255.0f ? 255 : lroundf(u));
  }
  return n;                                          // 26 bytes with air + light + UV (the legacy advert's limit), 14 with none
}

static void advertise(const uint8_t *svc, uint8_t len) {
  Bluefruit.Advertising.stop();
  Bluefruit.Advertising.clearData();
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addData(BLE_GAP_AD_TYPE_SERVICE_DATA, svc, len);
  Bluefruit.Advertising.setType(BLE_GAP_ADV_TYPE_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED);
  Bluefruit.Advertising.setInterval(ADV_INTERVAL_MS * 1000 / 625, ADV_INTERVAL_MS * 1000 / 625);
  Bluefruit.Advertising.restartOnDisconnect(false);
  Bluefruit.Advertising.start(ADV_BURST_S);          // broadcast this reading for a few seconds, then the radio sleeps
}

void setup() {
  Serial.begin(115200);

  pinMode(LED_RED, OUTPUT);   digitalWrite(LED_RED, HIGH);    // LEDs are active-low: off
  pinMode(LED_GREEN, OUTPUT); digitalWrite(LED_GREEN, HIGH);
  pinMode(LED_BLUE, OUTPUT);  digitalWrite(LED_BLUE, HIGH);

  pinMode(PIN_CAP_POWER, OUTPUT_H0H1);               // high drive: the probe pulls a few mA
  digitalWrite(PIN_CAP_POWER, LOW);
  pinMode(PIN_ROD_A, INPUT);
  pinMode(PIN_ROD_REF, INPUT);
  dht.begin();
  InternalFS.begin();
  loadCal();
#if LIGHT_SENSOR == LIGHT_I2C
  Wire.begin();
  shtOk  = sht4xBegin();
  ahtOk  = !shtOk && aht20Begin();
  vemlOk = veml.begin();
  if (vemlOk) veml.enable(false);                    // shut down until the first reading
  ltrOk  = ltr390Begin();
  airSrc = shtOk ? "sht4x" : ahtOk ? "aht20" : "dht11";
  Serial.printf("I2C sensors on D4/D5: air %s, light %s, UV %s\n", airSrc, lightName(), ltrOk ? "ltr390" : "none");
#else
  pinMode(PIN_LDR_POWER, OUTPUT_H0H1);
  digitalWrite(PIN_LDR_POWER, LOW);
#endif

  analogReference(AR_VDD4);                          // full scale = VDD -> ratiometric readings
  analogReadResolution(12);

  Bluefruit.begin();
  Bluefruit.autoConnLed(false);
  Bluefruit.setTxPower(TX_POWER_DBM);
  Bluefruit.setName("GW");

  ble_gap_addr_t addr = Bluefruit.getAddr();
  Serial.printf("GROUNDWELL node fw %s  address %02X:%02X:%02X:%02X:%02X:%02X\n", FW_VERSION,
                addr.addr[5], addr.addr[4], addr.addr[3], addr.addr[2], addr.addr[1], addr.addr[0]);
  if (calOk) Serial.printf("Factory calibration: air %u, water %u (sending moisture %%)\n", cal.air, cal.water);
  else       Serial.println("No factory calibration yet (sending the raw capacitive count). Send CAL AIR, then CAL WATER.");
}

void loop() {
  static uint32_t nextAt = 0;
  if (nextAt == 0) nextAt = millis();

  uint16_t cap  = readCapacitive();
  uint16_t rods = readRods();
  float vdd     = readVDD();
  float tempC = 0, rh = 0;
  bool airRead  = readAir(tempC, rh);
  static AirFilter airF = {false, false, 0, 0, 0, 0};
  bool airOk    = airRead && airFilter(airF, tempC, rh);
  float lux = 0;
  bool luxOk    = readLight(lux);
  float uvi = 0;
  bool uvOk     = readUV(uvi);
  int32_t capPct = calOk ? (int32_t)probeScaleX100(cap) : -1;

  uint8_t svc[31];
  uint8_t len = buildBTHome(svc, ++packetId, airOk, tempC, rh, luxOk, lux, vdd, capPct, cap, rods, uvOk, uvi);
  advertise(svc, len);
  uint32_t now = millis(), dt = lastSentAt ? now - lastSentAt : 0;
  lastSentAt = now;

  if (vdd < 1.6f) Serial.println("WARNING: VDD reading is impossible (the chip needs 1.7 V); please report this line.");
  char airTxt[48], luxTxt[48], capTxt[32];
  if (airOk)        snprintf(airTxt, sizeof airTxt, "air %.1f C %.0f%%RH", tempC, rh);
  else if (airRead) snprintf(airTxt, sizeof airTxt, "air: %.1f C %.0f%%RH held (jump)", tempC, rh);
  else              snprintf(airTxt, sizeof airTxt, "air: %s read failed", airSrc);
#if LIGHT_SENSOR == LIGHT_I2C
  if (luxOk) snprintf(luxTxt, sizeof luxTxt, uvOk ? "light %.0f lux uv %.1f" : "light %.0f lux", lux, uvi);
  else       snprintf(luxTxt, sizeof luxTxt, "light: none");
#else
  snprintf(luxTxt, sizeof luxTxt, "light %.0f lux (adc %u)", lux, ldrAdc);
#endif
  if (capPct >= 0) snprintf(capTxt, sizeof capTxt, "capacitive %4u = %5.1f%%", cap, capPct / 100.0f);
  else             snprintf(capTxt, sizeof capTxt, "capacitive %4u (uncal)", cap);
  Serial.printf("#%u dt=%lums  %s  rods %4u  %s  %s  VDD %.2f V\n", packetId, (unsigned long)dt, capTxt, rods, airTxt, luxTxt, vdd);

  // Absolute schedule: the next reading is due REPORT_MS after the last one was due, however long this
  // one took. The wait is a loop on the clock, so it can't end early, and it answers serial commands.
  uint32_t every = (fastUntil && (int32_t)(fastUntil - millis()) > 0) ? FAST_REPORT_MS : REPORT_MS;
  nextAt += every;
  if ((int32_t)(millis() - nextAt) >= 0) nextAt = millis() + every;       // fell behind (a 10 s calibration, say)
  while ((int32_t)(nextAt - millis()) > 0) {
    serviceSerial();
    uint32_t left = nextAt - millis();
    if ((int32_t)left <= 0) break;
    delay(Serial ? (left > 50 ? 50 : left) : left);  // FreeRTOS delay: the CPU sleeps here
  }
}

/*
  GROUNDWELL sensor node v0.13.1  (capacitive soil probe + air temperature/humidity + light + UV, one board)
  Board:  Seeed XIAO nRF52840 (plain or Sense)
          Arduino IDE: Boards Manager URL
            https://files.seeedstudio.com/arduino/package_seeeduino_boards_index.json
          install "Seeed nRF52 Boards" (NOT the "mbed-enabled" one), select "Seeed XIAO nRF52840"
          The Bluefruit BLE library ships with that core.
          Library Manager: install "DHT sensor library" by Adafruit (accept its "Adafruit Unified Sensor" dependency).
          Only if you set LIGHT_SENSOR to LIGHT_I2C: also install "Adafruit VEML7700 Library"
          (accept its "Adafruit BusIO" dependency). The SHT4x, AHT20 and LTR390 need no library.

  Updates over Bluetooth
    From the hub (v0.10): the hub carries this firmware inside its own. Every burst accepts a connection, and
    a hub that proves it holds this node's link key (HMAC-SHA256 over a fresh random challenge) may start an
    update. The node learns the key from the first hub that reaches it in the 10 minutes after power-up or a
    reset ("linking"); after that only that hub, or someone pressing reset, can link or update it.
    From a phone: for DFU_WINDOW_MS (10 min) after power-up or a reset, the node advertises continuously and
    accepts an update without the key: nRF Connect for Mobile > connect to "GW" > DFU > GroundwellNode.ino.zip.
    Serial command UNLINK forgets the link key.
  v0.12: the rod probe is gone (the capacitive probe is the soil sensor), so the packet is 3 bytes shorter and
  pins D2, D3 and D4 are free.
  v0.13 (battery prep): one reading a minute instead of every 30 s. The XIAO's unused 2 MB QSPI flash chip is
  put to sleep at boot (it idles at about 20 uA otherwise). The DHT11 can be powered from D3 instead of 3V3, so
  its 100 to 150 uA standby current only flows for about 3 s a minute: move its VCC wire to D3. Left on 3V3 it
  works as before. FAST now takes effect within 5 s. New serial command SETTLE shows how long the capacitive
  probe takes to settle after power-up.
  v0.13.1: fixes linking, and every write over Bluetooth that the node accepts. Its reply to an accepted write
  left out the written data, which the SoftDevice requires, so the reply was never sent and the hub waited 30 s:
  "key write got no reply in 30 s". Refusals were answered, which is why only a link that should succeed failed.
  A reply the SoftDevice rejects now counts in the node's self-report (rwerr).
  Diagnostics (v0.11): a read-only characteristic reports uptime, why it last restarted, readings sent,
  air-sensor read failures, sensors, calibration, link, update window, radio power, interval, connections
  and failed link-key attempts. The hub reads it at each check and shows it on http://plants.local/diag.

  What it does
    Every REPORT_MS (60 s) it reads the capacitive probe, air temperature/humidity and light, then broadcasts one
    BTHome v2 packet for ADV_BURST_S (6 s) and lets the radio sleep until the next reading:
      packet id, air temperature + humidity (best sensor found; skipped if a read fails or looks like a glitch),
      illuminance in lux, supply voltage (the hub and cloud derive battery level from it),
      capacitive probe: factory-calibrated moisture (BTHome moisture %, 0 = air, 100 = water)
                        or, before factory calibration, its raw count (0..4095, LOWER = wetter)
      UV index:         only with an LTR390 fitted
    Low power, on USB or battery alike: the CPU sleeps between readings, the capacitive probe and photoresistor
    are powered only while reading, the I2C sensors sit in standby between readings, the QSPI flash chip sleeps,
    the DHT11 is powered only while reading (if its VCC is on D3), and the radio is off except for the 6 s
    burst after each reading.

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
      SETTLE               capacitive probe output every 10 ms for 500 ms after power-up (v0.13): hold the
                           probe still in air or water, run it a few times, and see where it stops moving
    Every reply starts with INFO or CAL and ends in OK, PASS or FAIL, so tools/factory_cal.py can drive it.
    The calibration lives on the node, so a node ships calibrated with no plant and no hub settings.

  Wiring choice: pick one with LIGHT_SENSOR in the settings below.
    LIGHT_LDR (default)  GL5539 photoresistor on D7 + A5, DHT11 on D6
    LIGHT_I2C            I2C bus on D4 (SDA) / D5 (SCL) for any mix of SHT4x, AHT20, VEML7700, LTR390;
                         the photoresistor is not used (D5 is the I2C clock; the LTR390 or VEML7700
                         measures light); the DHT11 on D6 is still used if no SHT4x/AHT20 is found.
  The packet, hub and cloud are identical either way.

  Wiring
    Capacitive v2.0 probe (3-wire cable)
      black  GND   -> XIAO GND
      red    VCC   -> XIAO D1    (powered only while reading)
      yellow AOUT  -> XIAO D0/A0
    DHT11 module (3 pins, labels vary: S/DATA, +/VCC, -/GND)
      S / DATA     -> XIAO D6                                            (was D5 before v0.4)
      + / VCC      -> XIAO D3    for battery (v0.13): powered only while reading. 3V3 still works, always on.
      - / GND      -> XIAO GND
      Bare 4-pin DHT11 (no module board): add a 10 kOhm pull-up from DATA to the same pin as its VCC (D3 or 3V3).
      Never pull DATA up to 3V3 while VCC is on D3: the sensor would draw power through its data pin.
    GL5539 photoresistor (LIGHT_LDR, default)
      leg 1        -> XIAO D7    (powered only for a few ms per reading)
      leg 2        -> XIAO D5/A5
      1 kOhm       -> between XIAO D5/A5 and GND   (set LDR_FIXED_OHMS to 10000 if you fit a 10k)
      Either leg; polarity doesn't matter. Face it up at leaf height.
    I2C breakouts (LIGHT_I2C): SHT40 (Adafruit 4885), AHT20 (4566), VEML7700 (4162), LTR390 (4831), any mix.
      All of them share the same four wires (daisy-chain STEMMA QT boards, or wire them in parallel):
      SDA          -> XIAO D4    (the XIAO's I2C pins)
      SCL          -> XIAO D5
      VIN / VCC    -> XIAO 3V3
      GND          -> XIAO GND
      Light sensors: face up with a clear view of the window. Temperature/humidity: in open air at the plant,
      away from the board and out of direct sun. Anything not found at boot is simply left out of the packet.
    Power: USB-C or a battery; the firmware runs the same low-power cycle on both. If you fit a coin cell to
    the BAT pads, remove it before plugging USB in (the onboard charger would try to charge it).

  Test node on an Arduino Nano ESP32 (bench use, until more XIAOs arrive)
    The same sketch also builds for the Arduino Nano ESP32, with either board package: "esp32" 3.x by
    Espressif (the one the hub uses) or Arduino's own "Arduino ESP32 Boards" (2.0.x), board "Arduino Nano
    ESP32", Pin Numbering "By Arduino pin (default)". It sends the same BTHome
    packet, so the hub shows it like any other node, and it takes the same serial commands, so
    tools/factory_cal.py calibrates it the same way. What's different:
      - Its adverts are non-connectable. The hub reads them but never connects, so it never links or
        updates this node over Bluetooth: flash it over USB.
      - It runs on USB only and never sleeps (tens of mA), so it says nothing about battery life.
        It reports a fixed 3.30 V supply, because the ESP32 can't measure its own rail.
      - Its ADC isn't referenced to the supply like the nRF52840's. The sketch reads calibrated
        millivolts and scales them to the 3.3 V rail, so the same 0..4095 math holds, but the input
        tops out near 3.1 V: the photoresistor saturates around 70,000 lux. Run factory calibration on it;
        the capacitive probe then reads on the same scale.
    Nano ESP32 wiring (every analog input on ADC1, A0..A3; D and A pins are separate pins on the Nano)
      Capacitive   black GND -> GND, red VCC -> D2, yellow AOUT -> A0
      DHT11        DATA -> D6, VCC -> 3V3 or D3 (never 5V: the Nano's pins are 3.3 V only), GND -> GND
      GL5539       leg 1 -> D7, leg 2 -> A3, 1 kOhm between A3 and GND
      I2C sensors  (LIGHT_I2C) SDA -> A4, SCL -> A5
*/

static const char *FW_VERSION = "0.13.1";

// ---------------- light sensor choice ----------------
#define LIGHT_LDR      1                         // GL5539 photoresistor + DHT11, no I2C (the parts in hand)
#define LIGHT_I2C      2                         // I2C sensors on D4/D5: SHT4x / AHT20 / VEML7700 / LTR390
#define LIGHT_VEML7700 LIGHT_I2C                 // older name for the same wiring
#define LIGHT_SENSOR   LIGHT_LDR                 // <-- change to LIGHT_I2C when you fit I2C sensors

// ---------------- board ----------------
#if defined(ARDUINO_ARCH_NRF52)
#define GW_NRF52 1                               // Seeed XIAO nRF52840: the product node
#define GW_ESP32 0
#elif defined(ARDUINO_ARCH_ESP32)
#define GW_NRF52 0
#define GW_ESP32 1                               // Arduino Nano ESP32: bench test node (see the header)
#if !defined(ARDUINO_NANO_ESP32)
#warning "ESP32 build: the pins below are for the Arduino Nano ESP32; check them against your board"
#endif
#else
#error "GroundwellNode builds for the Seeed XIAO nRF52840, or as a test node for the Arduino Nano ESP32"
#endif

#if GW_NRF52
#include <bluefruit.h>
#include "gw_hmac.h"
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
using namespace Adafruit_LittleFS_Namespace;
#else
#include <BLEDevice.h>
#include <BLEAdvertising.h>
#include <Preferences.h>
#endif
#include <DHT.h>
#if LIGHT_SENSOR == LIGHT_I2C
#include <Wire.h>
#include <Adafruit_VEML7700.h>
#endif

// ---------------- settings ----------------
static const uint32_t REPORT_MS       = 60000; // new reading every 60 s (soil changes over hours; v0.9 to v0.12: 30 s)
static const uint32_t FAST_REPORT_MS  = 5000;  // after a FAST command, for FAST_FOR_MS
static const uint32_t FAST_FOR_MS     = 300000;
static const uint16_t ADV_INTERVAL_MS = 250;   // within a burst, repeat the packet every 250 ms ...
static const uint8_t  ADV_BURST_S     = 6;     // ... for 6 s (~24 copies), then the radio sleeps
static const float    AIR_JUMP_C      = 3.0f;  // air readings that jump more than this from the last one ...
static const float    AIR_JUMP_RH     = 15.0f;
static const float    AIR_CONFIRM_C   = 1.5f;  // ... are held until the next reading lands within this of them
static const float    AIR_CONFIRM_RH  = 6.0f;
static const int8_t   TX_POWER_DBM    = 4;     // 0 or 4; raise to 8 if the hub is far away
static const uint32_t DFU_WINDOW_MS   = 10UL * 60 * 1000;   // Bluetooth updates accepted this long after a reset (0 = never)
static const uint16_t CAP_SETTLE_MS   = 150;   // capacitive probe power-up settle
static const uint8_t  OVERSAMPLE      = GW_ESP32 ? 64 : 16;   // the ESP32's ADC is noisier: average more
#define DHT_TYPE DHT11                           // DHT22 if you upgrade the sensor
// DHT11 powered from PIN_DHT_POWER (v0.13). Its datasheet asks for 1 s after power-up before the first command,
// and each read returns the sensor's previous measurement, so the first read after power-up may be stale.
// The node reads twice and sends the second; the serial line shows the first too ("1st ..."). If the two
// always agree on your sensors, set DHT_SECOND_MS to 0: the DHT11 is then on for about 1.2 s a minute, not 3.3 s.
static const uint16_t DHT_WARMUP_MS   = 1200;
static const uint16_t DHT_SECOND_MS   = 2100;  // the DHT library allows one real read per 2 s

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
#if GW_NRF52
static const int PIN_CAP_AOUT  = A0;   // D0
static const int PIN_CAP_POWER = D1;
static const int PIN_DHT       = D6;
static const int PIN_DHT_POWER = D3;   // DHT11 VCC for battery use (v0.13); nothing happens if VCC stays on 3V3
#if LIGHT_SENSOR != LIGHT_I2C
static const int PIN_LDR_POWER = D7;
static const int PIN_LDR_SENSE = A5;   // D5
#endif
__attribute__((unused)) static const char *I2C_PINS = "D4/D5";
#define OUTPUT_STRONG OUTPUT_H0H1                // high drive: the capacitive probe pulls a few mA
#else
// Arduino Nano ESP32 (Arduino pin numbering). Every analog input is on ADC1 (A0..A3); A4/A5 are on ADC2
// and are the I2C pins. D0/D1 are the Nano's UART, so the probe power moves to D2.
static const int PIN_CAP_AOUT  = A0;
static const int PIN_CAP_POWER = D2;
static const int PIN_DHT       = D6;
static const int PIN_DHT_POWER = D3;
#if LIGHT_SENSOR != LIGHT_I2C
static const int PIN_LDR_POWER = D7;
static const int PIN_LDR_SENSE = A3;
#endif
__attribute__((unused)) static const char *I2C_PINS = "A4/A5";
#define OUTPUT_STRONG OUTPUT                     // ESP32 GPIOs drive about 20 mA by default
static const uint16_t ESP_VDD_MV = 3300;         // the Nano's 3.3 V rail: ADC scale and the reported supply
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

// Hub link (v0.10): the key the hub proved or set, kept in flash like the calibration (XIAO only)
#if GW_NRF52
static const char *KEY_FILE = "/gwkey.bin";
static const uint32_t KEY_MAGIC = 0x4B455931;        // "KEY1"
struct KeyData { uint32_t magic; uint8_t key[16]; uint32_t check; };
static uint8_t linkKey[16];
static volatile bool keyDirty = false, authed = false;
static uint8_t nonce[16];
#endif
static volatile bool keyOk = false;                  // stays false on the ESP32 test node: it never links
static float lastVdd = 0;
static SemaphoreHandle_t wakeSem;                    // lets a Bluetooth event wake the main loop early
// Diagnostics (v0.11)
static uint32_t readings = 0, airFails = 0, connects = 0, authFails = 0;
static uint32_t fastUntil = 0;                        // FAST command: 5 s readings until this millis()
static bool fastKick = false;                         // FAST just arrived: bring the next reading forward (v0.13)
static uint32_t dhtOnAt = 0;                          // when the DHT11's power pin went high this cycle
static bool dht1Ok = false;                           // the first DHT11 read after power-up, for the serial line
static float dht1T = 0, dht1H = 0;
// Declared here, before any function: the Arduino IDE puts a prototype of every function above the first one.
struct AirFilter { bool have, pend; float t, h, pt, ph; };

// ---------------- measurements ----------------
// One ADC sample as 0..4095 of the supply rail, the scale every reading below is written for.
static uint16_t adcRead(int pin) {
#if GW_NRF52
  return (uint16_t)analogRead(pin);                  // AR_VDD4: already ratiometric to VDD
#else
  // The ESP32's ADC has its own reference, so read calibrated millivolts and scale them to the 3.3 V rail.
  // The input range ends near 3.1 V, so readings close to the rail saturate a little below 4095.
  uint32_t c = ((uint32_t)analogReadMilliVolts(pin) * 4095u + ESP_VDD_MV / 2) / ESP_VDD_MV;
  return (uint16_t)(c > 4095u ? 4095u : c);
#endif
}

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

// Supply voltage through the SAADC's internal VDD channel (no divider, no leakage).
// The ESP32 test node can't measure its own rail, so it reports the nominal 3.30 V (it runs on USB).
static float readVDD() {
#if GW_ESP32
  return ESP_VDD_MV / 1000.0f;
#else
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
#endif
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

// ---------------- battery helpers (v0.13) ----------------
// The CPU sleeps in delay() (FreeRTOS idle on the XIAO), so these waits cost almost nothing.
static void sleepUntil(uint32_t t) {
  int32_t left = (int32_t)(t - millis());
  if (left > 0) delay((uint32_t)left);
}

// True when the air reading comes from the DHT11 (no SHT4x or AHT20 found on I2C).
static bool dhtInUse() {
#if LIGHT_SENSOR == LIGHT_I2C
  return !shtOk && !ahtOk;
#else
  return true;
#endif
}

// DHT11 power from PIN_DHT_POWER. If its VCC is still on 3V3 this does nothing harmful: the pin just has
// nothing on it, and the sensor stays powered as before.
static void dhtPowerOn() {
  digitalWrite(PIN_DHT_POWER, HIGH);
  dhtOnAt = millis();
}
static void dhtPowerOff() {
  digitalWrite(PIN_DHT_POWER, LOW);
  pinMode(PIN_DHT, INPUT);    // the DHT library leaves its internal pull-up on, which would feed an unpowered sensor
}

// This cycle's air reading. With the DHT11: wait out its power-up time (the soil and light readings already ran
// meanwhile), read once, read again DHT_SECOND_MS later and send that one, then power it off.
static bool readAirCycle(float &tempC, float &rh) {
  if (!dhtInUse()) return readAir(tempC, rh);
  sleepUntil(dhtOnAt + DHT_WARMUP_MS);
  dht1Ok = readDHT(dht1T, dht1H);
  bool ok = dht1Ok;
  tempC = dht1T; rh = dht1H;
  if (DHT_SECOND_MS) {
    delay(DHT_SECOND_MS);
    ok = readDHT(tempC, rh);
  }
  dhtPowerOff();
  return ok;
}

#if GW_NRF52
// The XIAO nRF52840's 2 MB QSPI flash (P25Q16H) isn't used: calibration and the link key live in the nRF52's
// own flash. Left alone it idles in standby at about 20 uA, more than the rest of the sleeping board. Send it
// Deep Power-Down (0xB9) over plain SPI on its pins (P0.21 SCK, P0.25 CS, P0.20 IO0, P0.22 IO2/WP#,
// P0.23 IO3/HOLD#), then leave it deselected. It sleeps until the next power cycle; every boot sends it again.
static void flashSleep() {
  const uint32_t CS = 25, SCK = 21, IO0 = 20, IO2 = 22, IO3 = 23;
  const uint32_t OUT = (GPIO_PIN_CNF_DIR_Output << GPIO_PIN_CNF_DIR_Pos) |
                       (GPIO_PIN_CNF_INPUT_Disconnect << GPIO_PIN_CNF_INPUT_Pos);
  NRF_P0->OUTSET = (1UL << CS) | (1UL << IO2) | (1UL << IO3);   // deselected, write-protect and hold inactive
  NRF_P0->OUTCLR = (1UL << SCK) | (1UL << IO0);
  NRF_P0->PIN_CNF[CS] = OUT; NRF_P0->PIN_CNF[SCK] = OUT; NRF_P0->PIN_CNF[IO0] = OUT;
  NRF_P0->PIN_CNF[IO2] = OUT; NRF_P0->PIN_CNF[IO3] = OUT;
  delayMicroseconds(50);
  NRF_P0->OUTCLR = 1UL << CS;
  delayMicroseconds(1);
  for (int8_t b = 7; b >= 0; b--) {                 // SPI mode 0, MSB first: set data, then a rising clock edge
    if ((0xB9 >> b) & 1) NRF_P0->OUTSET = 1UL << IO0; else NRF_P0->OUTCLR = 1UL << IO0;
    delayMicroseconds(1);
    NRF_P0->OUTSET = 1UL << SCK;
    delayMicroseconds(1);
    NRF_P0->OUTCLR = 1UL << SCK;
  }
  delayMicroseconds(1);
  NRF_P0->OUTSET = 1UL << CS;                        // the command runs when CS goes high
  delayMicroseconds(20);
}
#endif

// SETTLE: the capacitive probe's output every 10 ms for 500 ms after power-up, to choose CAP_SETTLE_MS from
// data. Prints "SETTLE ms:count ..." and the first time after which every sample stays within 8 counts of the
// 500 ms value. CAP_SETTLE_MS is 150 and factory calibration was taken at 150: if you shorten it, recalibrate.
static void settleTest() {
  static const uint8_t N = 51;
  uint16_t v[N];
  digitalWrite(PIN_CAP_POWER, HIGH);
  uint32_t t0 = millis();
  for (uint8_t i = 0; i < N; i++) {
    sleepUntil(t0 + 10UL * i);
    v[i] = avgRead(PIN_CAP_AOUT, OVERSAMPLE);
  }
  digitalWrite(PIN_CAP_POWER, LOW);
  int settled = 0;
  for (int i = N - 1; i >= 0; i--) {
    if (abs((int)v[i] - (int)v[N - 1]) > 8) { settled = (i + 1) * 10; break; }
  }
  Serial.print("SETTLE ms:count");
  for (uint8_t i = 0; i < N; i++) Serial.printf(" %u:%u", 10u * i, v[i]);
  Serial.printf("\nSETTLE settled_ms=%d final=%u now_ms=%u OK\n", settled, v[N - 1], (unsigned)CAP_SETTLE_MS);
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

// ---------------- flash storage ----------------
// XIAO: files in the nRF52's internal LittleFS. Nano ESP32: NVS (Preferences), because the Nano's default
// partition table has no LittleFS partition. Same records, same checks, either way.
#if GW_NRF52
static void storeBegin() { InternalFS.begin(); }
static bool storeRead(const char *path, void *buf, size_t n) {
  File f(InternalFS);
  if (!f.open(path, FILE_O_READ)) return false;
  bool ok = f.read(buf, n) == (int)n;
  f.close();
  return ok;
}
static bool storeWrite(const char *path, const void *buf, size_t n) {
  InternalFS.remove(path);                           // FILE_O_WRITE appends, so start fresh
  File f(InternalFS);
  if (!f.open(path, FILE_O_WRITE)) return false;
  bool ok = f.write((const uint8_t *)buf, n) == n;
  f.close();
  return ok;
}
static void storeRemove(const char *path) { InternalFS.remove(path); }
#else
static Preferences prefs;
static const char *prefKey(const char *path) { return path[0] == '/' ? path + 1 : path; }   // NVS keys: 15 chars max
static void storeBegin() { prefs.begin("groundwell", false); }
static bool storeRead(const char *path, void *buf, size_t n) {
  return prefs.isKey(prefKey(path)) && prefs.getBytes(prefKey(path), buf, n) == n;
}
static bool storeWrite(const char *path, const void *buf, size_t n) { return prefs.putBytes(prefKey(path), buf, n) == n; }
static void storeRemove(const char *path) { if (prefs.isKey(prefKey(path))) prefs.remove(prefKey(path)); }
#endif

// ---------------- factory calibration ----------------
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

static const char *lightName() {
#if LIGHT_SENSOR == LIGHT_I2C
  return vemlOk ? "veml7700" : ltrOk ? "ltr390" : "none";
#else
  return "ldr";
#endif
}

// Bluetooth address as the hub shows it: "AA:BB:CC:DD:EE:FF".
static void addrText(char *out) {
#if GW_NRF52
  ble_gap_addr_t a = Bluefruit.getAddr();
  snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a.addr[5], a.addr[4], a.addr[3], a.addr[2], a.addr[1], a.addr[0]);
#else
  strlcpy(out, BLEDevice::getAddress().toString().c_str(), 18);
  for (char *c = out; *c; c++) *c = (char)toupper((unsigned char)*c);
#endif
}

static void printInfo() {
  char mac[18];
  addrText(mac);
  Serial.printf("INFO fw=%s mac=%s interval_ms=%lu light=%s ", FW_VERSION, mac, (unsigned long)REPORT_MS, lightName());
#if LIGHT_SENSOR == LIGHT_I2C
  Serial.printf("air=%s uv=%s ", airSrc, ltrOk ? "ltr390" : "none");
#else
  Serial.printf("air=%s uv=none ", airSrc);
#endif
  Serial.printf("link=%s ", keyOk ? "yes" : "no");
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
  else if (!strcmp(line, "CAL CLEAR")) { storeRemove(CAL_FILE); calOk = false; pendAir = pendWater = 0; Serial.println("CAL CLEARED OK"); }
#if GW_NRF52
  else if (!strcmp(line, "UNLINK"))    { storeRemove(KEY_FILE); keyOk = false; updateInfo(); Serial.println("UNLINKED OK"); }
#else
  else if (!strcmp(line, "UNLINK"))    Serial.println("UNLINKED OK (this test node never links to a hub)");
#endif
  else if (!strcmp(line, "SETTLE"))    settleTest();
  else if (!strcmp(line, "FAST"))      { fastUntil = millis() + FAST_FOR_MS; fastKick = true; Serial.printf("FAST interval_ms=%lu for_s=%lu OK\n", (unsigned long)FAST_REPORT_MS, (unsigned long)(FAST_FOR_MS / 1000)); }
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
//         | 0C mV (u16) | [14 moisture*100 (u16), calibrated capacitive] | [3D cap raw (u16), uncalibrated]
//         | [46 UV index*10 (u8)]
// v0.12: no rod probe count (it was a second 3D object after the capacitive probe).
// capPct is the probe scale x100 (0..10000) when calibrated, or -1 to send the raw capacitive count instead.
// Battery % (0x01) is no longer sent (v0.9): the supply voltage carries the same information, and the
// two bytes make room for the UV index inside the 31-byte legacy advert.
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
// ---- end BTHome packet (host-tested: tools/bthome_roundtrip_test.cpp) ----

#if GW_NRF52
BLEDis bledis;                                       // device information: shows the firmware version in nRF Connect

static bool dfuWindowOpen() { return DFU_WINDOW_MS && millis() < DFU_WINDOW_MS; }

// ---------------- link to the hub (v0.10) ----------------
// GROUNDWELL service 6a1c0001-5f2b-4c1e-9d55-67726f756e64 ("ground"), characteristics ...02 to ...05.
#define GW_UUID(n) {0x64, 0x6e, 0x75, 0x6f, 0x72, 0x67, 0x55, 0x9d, 0x1e, 0x4c, 0x2b, 0x5f, n, 0x00, 0x1c, 0x6a}
static const uint8_t UUID_GW_SVC[16] = GW_UUID(0x01), UUID_GW_INFO[16] = GW_UUID(0x02), UUID_GW_NONCE[16] = GW_UUID(0x03),
                     UUID_GW_AUTH[16] = GW_UUID(0x04), UUID_GW_KEY[16] = GW_UUID(0x05);
BLEService gwSvc(UUID_GW_SVC);
BLECharacteristic gwInfo(UUID_GW_INFO);              // read: "0.10.0;l=1;w=0;v=3.29" (firmware; linked; window open; volts)
BLECharacteristic gwNonce(UUID_GW_NONCE);            // read: 16 random bytes, new for every connection and attempt
BLECharacteristic gwAuth(UUID_GW_AUTH);              // write: first 16 bytes of HMAC-SHA256(link key, nonce)
BLECharacteristic gwKey(UUID_GW_KEY);                // write: a new 16-byte link key (in the window, or once authenticated)
static const uint8_t UUID_GW_DIAG[16] = GW_UUID(0x06);
BLECharacteristic gwDiag(UUID_GW_DIAG);              // read: "up=..;rst=..;n=..;..." node health for the hub's diagnostics page


static uint32_t keyCheck(const KeyData &k) {
  uint32_t h = 2166136261u;
  const uint8_t *p = (const uint8_t *)&k;
  for (size_t i = 0; i < offsetof(KeyData, check); i++) { h ^= p[i]; h *= 16777619u; }
  return h;
}

static void loadKey() {
  keyOk = false;
  KeyData k;
  if (storeRead(KEY_FILE, &k, sizeof k) && k.magic == KEY_MAGIC && k.check == keyCheck(k)) { memcpy(linkKey, k.key, 16); keyOk = true; }
}

static bool saveKey() {
  KeyData k = {KEY_MAGIC, {0}, 0};
  memcpy(k.key, linkKey, 16);
  k.check = keyCheck(k);
  return storeWrite(KEY_FILE, &k, sizeof k);
}

static void newNonce() {
  if (sd_rand_application_vector_get(nonce, sizeof nonce) != NRF_SUCCESS) {
    // Random pool empty (rare): stretch the last nonce with the clock instead of reusing it.
    uint8_t seed[24], out[32];
    memcpy(seed, nonce, 16);
    uint32_t t[2] = {micros(), millis()};
    memcpy(seed + 16, t, 8);
    GwSha256 h; gw_sha256_init(&h); gw_sha256_update(&h, seed, sizeof seed); gw_sha256_final(&h, out);
    memcpy(nonce, out, 16);
  }
  gwNonce.write(nonce, sizeof nonce);
}

static const char *resetReasonText() {
  uint32_t r = readResetReason();                  // NRF_POWER->RESETREAS, saved by the core at startup
  if (r & 0x1) return "reset-pin";
  if (r & 0x2) return "watchdog";
  if (r & 0x4) return "software";                  // includes the restart after an update
  if (r & 0x8) return "lockup";
  if (r & 0x10000) return "wake-from-off";
  if (r & 0x100000) return "usb-power";
  return "power-on";
}

static void updateDiag() {
  char b[160];
  uint32_t winLeft = dfuWindowOpen() ? (DFU_WINDOW_MS - millis()) / 1000 : 0;
  bool fast = fastUntil && (int32_t)(fastUntil - millis()) > 0;
  snprintf(b, sizeof b, "up=%lu;rst=%s;n=%lu;air=%s;airfail=%lu;light=%s;cal=%d;link=%d;win=%lu;tx=%d;ivl=%lu;con=%lu;authfail=%lu;rwerr=%lu",
           (unsigned long)(millis() / 1000), resetReasonText(), (unsigned long)readings, airSrc, (unsigned long)airFails, lightName(),
           calOk ? 1 : 0, keyOk ? 1 : 0, (unsigned long)winLeft, (int)TX_POWER_DBM,
           (unsigned long)((fast ? FAST_REPORT_MS : REPORT_MS) / 1000), (unsigned long)connects, (unsigned long)authFails,
           (unsigned long)replyErrs);
  gwDiag.write(b);
}

static void updateInfo() {
  char b[40];
  snprintf(b, sizeof b, "%s;l=%d;w=%d;v=%.2f", FW_VERSION, keyOk ? 1 : 0, dfuWindowOpen() ? 1 : 0, lastVdd);
  gwInfo.write(b);
}

static bool plainWrite(ble_gatts_evt_write_t *r) {
  return r->op != BLE_GATTS_OP_PREP_WRITE_REQ && r->op != BLE_GATTS_OP_EXEC_WRITE_REQ_NOW && r->op != BLE_GATTS_OP_EXEC_WRITE_REQ_CANCEL;
}

// Answer a write that waits for our authorization. An accepted write must hand its data back with update set:
// the SoftDevice API says the bit "must always be set" for writes, and without it the call fails and no reply
// goes out at all (v0.13.1; before, every accepted key write and key proof left the hub waiting 30 s).
static uint32_t replyErrs = 0;
static void replyWrite(uint16_t conn, bool ok, const ble_gatts_evt_write_t *r) {
  ble_gatts_rw_authorize_reply_params_t reply = {.type = BLE_GATTS_AUTHORIZE_TYPE_WRITE};
  reply.params.write.gatt_status = ok ? BLE_GATT_STATUS_SUCCESS : BLE_GATT_STATUS_ATTERR_INSUF_AUTHORIZATION;
  if (ok) {
    reply.params.write.update = 1;
    reply.params.write.offset = r->offset;
    reply.params.write.len    = r->len;
    reply.params.write.p_data = r->data;
  }
  if (sd_ble_gatts_rw_authorize_reply(conn, &reply) != NRF_SUCCESS) replyErrs++;
}

static void authWriteCb(uint16_t conn, BLECharacteristic *, ble_gatts_evt_write_t *r) {
  bool ok = false;
  if (plainWrite(r) && keyOk && r->len == 16) {
    uint8_t mac[32];
    gw_hmac_sha256(linkKey, 16, nonce, 16, mac);
    ok = gw_equal(mac, r->data, 16);
  }
  replyWrite(conn, ok, r);
  if (ok) authed = true; else authFails++;
  newNonce();                                        // one guess per nonce
}

static void keyWriteCb(uint16_t conn, BLECharacteristic *, ble_gatts_evt_write_t *r) {
  bool ok = plainWrite(r) && r->len == 16 && (dfuWindowOpen() || authed);
  if (ok) {
    memcpy(linkKey, r->data, 16);
    keyOk = true;
    keyDirty = true;                                 // saved to flash by the main loop, woken right away
    xSemaphoreGive(wakeSem);
  }
  replyWrite(conn, ok, r);
  updateInfo();
}

static void connectCb(uint16_t) { authed = false; connects++; newNonce(); updateInfo(); updateDiag(); }
static void disconnectCb(uint16_t, uint8_t) { authed = false; newNonce(); }

static void serviceLink() {
  if (!keyDirty) return;
  keyDirty = false;
  Serial.println(saveKey() ? "Linked to a hub (link key saved)" : "Link key couldn't be saved");
}

// Same service and behavior as Bluefruit's BLEDfu (legacy DFU, buttonless start), with one change: the start
// command is only accepted in the window after a reset, or from a connection that authenticated with the link key.
static const uint8_t UUID_DFU_SVC[16]  = {0x23, 0xD1, 0xBC, 0xEA, 0x5F, 0x78, 0x23, 0x15, 0xDE, 0xEF, 0x12, 0x12, 0x30, 0x15, 0x00, 0x00};
static const uint8_t UUID_DFU_CTRL[16] = {0x23, 0xD1, 0xBC, 0xEA, 0x5F, 0x78, 0x23, 0x15, 0xDE, 0xEF, 0x12, 0x12, 0x31, 0x15, 0x00, 0x00};
static const uint8_t UUID_DFU_PKT[16]  = {0x23, 0xD1, 0xBC, 0xEA, 0x5F, 0x78, 0x23, 0x15, 0xDE, 0xEF, 0x12, 0x12, 0x32, 0x15, 0x00, 0x00};
static const uint8_t UUID_DFU_REV[16]  = {0x23, 0xD1, 0xBC, 0xEA, 0x5F, 0x78, 0x23, 0x15, 0xDE, 0xEF, 0x12, 0x12, 0x34, 0x15, 0x00, 0x00};
extern "C" void bootloader_util_app_start(uint32_t start_addr);
BLEService dfuSvc(UUID_DFU_SVC);
BLECharacteristic dfuCtrl(UUID_DFU_CTRL);

static uint16_t dfuCrc16(const uint8_t *p, uint8_t n) {
  uint16_t crc = 0xFFFF;
  while (n--) { uint8_t x = crc >> 8 ^ *p++; x ^= x >> 4; crc = (crc << 8) ^ ((uint16_t)(x << 12)) ^ ((uint16_t)(x << 5)) ^ ((uint16_t)x); }
  return crc;
}

static void dfuCtrlCb(uint16_t conn_hdl, BLECharacteristic *chr, ble_gatts_evt_write_t *request) {
  if (request->handle != chr->handles().value_handle || !plainWrite(request)) return;
  BLEConnection *conn = Bluefruit.Connection(conn_hdl);
  ble_gatts_rw_authorize_reply_params_t reply = {.type = BLE_GATTS_AUTHORIZE_TYPE_WRITE};
  if (!(dfuWindowOpen() || authed)) {
    reply.params.write.gatt_status = BLE_GATT_STATUS_ATTERR_INSUF_AUTHORIZATION;
    sd_ble_gatts_rw_authorize_reply(conn_hdl, &reply);
    return;
  }
  if (!chr->notifyEnabled(conn_hdl)) {
    reply.params.write.gatt_status = BLE_GATT_STATUS_ATTERR_CPS_CCCD_CONFIG_ERROR;
    sd_ble_gatts_rw_authorize_reply(conn_hdl, &reply);
    return;
  }
  reply.params.write.gatt_status = BLE_GATT_STATUS_SUCCESS;
  reply.params.write.update = 1;                     // an accepted write must hand its data back (v0.13.1)
  reply.params.write.offset = request->offset;
  reply.params.write.len    = request->len;
  reply.params.write.p_data = request->data;
  if (sd_ble_gatts_rw_authorize_reply(conn_hdl, &reply) != NRF_SUCCESS) replyErrs++;
  if (request->data[0] != 1) return;                 // 1 = START DFU

  // Hand the connection data to the bootloader (at 0x20007F80) so it keeps the same address and the client
  // can reconnect. (The hub links and updates in separate connections, so a new link key is already saved.)
  typedef struct { ble_gap_addr_t addr; ble_gap_irk_t irk; ble_gap_enc_key_t enc_key; uint8_t sys_attr[8]; uint16_t crc16; } peer_data_t;
  static_assert(offsetof(peer_data_t, crc16) == 60, "bootloader peer data layout");
  peer_data_t *peer = (peer_data_t *)(0x20007F80UL);
  varclr(peer);
  uint16_t sysattr_len = sizeof(peer->sys_attr);
  sd_ble_gatts_sys_attr_get(conn_hdl, peer->sys_attr, &sysattr_len, BLE_GATTS_SYS_ATTR_FLAG_SYS_SRVCS);
  peer->addr = conn->getPeerAddr();
  if (conn->secured()) {
    bond_keys_t bkeys;
    if (conn->loadBondKey(&bkeys)) { peer->addr = bkeys.peer_id.id_addr_info; peer->irk = bkeys.peer_id.id_info; peer->enc_key = bkeys.own_enc; }
  }
  peer->crc16 = dfuCrc16((uint8_t *)peer, offsetof(peer_data_t, crc16));
  Bluefruit.Advertising.restartOnDisconnect(false);
  conn->disconnect();
  sd_power_gpregret_clr(0, 0xFF);
  if (sd_power_gpregret_set(0, 0xB1) != NRF_SUCCESS) return;   // 0xB1 = DFU over the air
  if (sd_softdevice_disable() != NRF_SUCCESS) return;
  NVIC->ICER[0] = 0xFFFFFFFF; NVIC->ICPR[0] = 0xFFFFFFFF;
#if defined(__NRF_NVIC_ISER_COUNT) && __NRF_NVIC_ISER_COUNT == 2
  NVIC->ICER[1] = 0xFFFFFFFF; NVIC->ICPR[1] = 0xFFFFFFFF;
#endif
  if (sd_softdevice_vector_table_base_set(NRF_UICR->NRFFW[0]) != NRF_SUCCESS) return;
  __set_CONTROL(0);
  bootloader_util_app_start(NRF_UICR->NRFFW[0]);
}

static void beginLinkServices() {
  gwSvc.begin();
  gwInfo.setProperties(CHR_PROPS_READ);   gwInfo.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);  gwInfo.setMaxLen(40); gwInfo.begin();
  gwNonce.setProperties(CHR_PROPS_READ);  gwNonce.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS); gwNonce.setFixedLen(16); gwNonce.begin();
  gwAuth.setProperties(CHR_PROPS_WRITE);  gwAuth.setPermission(SECMODE_NO_ACCESS, SECMODE_OPEN);  gwAuth.setFixedLen(16);
  gwAuth.setWriteAuthorizeCallback(authWriteCb); gwAuth.begin();
  gwKey.setProperties(CHR_PROPS_WRITE);   gwKey.setPermission(SECMODE_NO_ACCESS, SECMODE_OPEN);   gwKey.setFixedLen(16);
  gwKey.setWriteAuthorizeCallback(keyWriteCb); gwKey.begin();
  gwDiag.setProperties(CHR_PROPS_READ);   gwDiag.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);  gwDiag.setMaxLen(160); gwDiag.begin();

  dfuSvc.begin();
  BLECharacteristic pkt(UUID_DFU_PKT);
  pkt.setTempMemory(); pkt.setProperties(CHR_PROPS_WRITE_WO_RESP); pkt.setMaxLen(20); pkt.begin();
  dfuCtrl.setProperties(CHR_PROPS_WRITE | CHR_PROPS_NOTIFY); dfuCtrl.setMaxLen(23);
  dfuCtrl.setWriteAuthorizeCallback(dfuCtrlCb); dfuCtrl.begin();
  BLECharacteristic rev(UUID_DFU_REV);
  rev.setTempMemory(); rev.setProperties(CHR_PROPS_READ); rev.setFixedLen(2); rev.begin(); rev.write16(0x0001);   // "application mode"

  Bluefruit.Periph.setConnectCallback(connectCb);
  Bluefruit.Periph.setDisconnectCallback(disconnectCb);
  newNonce();
  updateInfo();
  updateDiag();
}

static void advertise(const uint8_t *svc, uint8_t len) {
  // Every burst accepts a connection (v0.10), so the hub can check and update the node; what a connection
  // may do is decided by the link key and the window after a reset. In that window the node advertises
  // continuously, for phones. While connected, the reading still goes out as a non-connectable burst.
  updateInfo();
  updateDiag();
  bool connectable = Bluefruit.connected() == 0;
  bool continuous = connectable && dfuWindowOpen();
  Bluefruit.Advertising.stop();
  Bluefruit.Advertising.clearData();
  Bluefruit.ScanResponse.clearData();
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addData(BLE_GAP_AD_TYPE_SERVICE_DATA, svc, len);
  if (connectable) Bluefruit.ScanResponse.addName();    // "GW"

  Bluefruit.Advertising.setType(connectable ? BLE_GAP_ADV_TYPE_CONNECTABLE_SCANNABLE_UNDIRECTED
                                            : BLE_GAP_ADV_TYPE_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED);
  Bluefruit.Advertising.setInterval(ADV_INTERVAL_MS * 1000 / 625, ADV_INTERVAL_MS * 1000 / 625);
  Bluefruit.Advertising.restartOnDisconnect(false);
  Bluefruit.Advertising.start(continuous ? 0 : ADV_BURST_S);    // 0 = until the next reading replaces it
}

#else  // GW_ESP32: Arduino Nano ESP32 test node
// Broadcast only. The adverts are non-connectable, so the hub reads the packet but never connects:
// no link key, no Bluetooth updates (the hub only checks nodes whose adverts accept connections).
static BLEAdvertising *adv = nullptr;
static uint32_t burstEndsAt = 0;                     // millis() when the current burst stops (0 = radio quiet)

static void bleBegin() {
  BLEDevice::init("GW");
  BLEDevice::setPower(ESP_PWR_LVL_P3);               // +3 dBm, the nearest ESP32 step to the XIAO's +4
  adv = BLEDevice::getAdvertising();
  adv->setScanResponse(false);
#if defined(CONFIG_BLUEDROID_ENABLED)
  adv->setAdvertisementType(ADV_TYPE_NONCONN_IND);
#else
  adv->setAdvertisementType(BLE_GAP_CONN_MODE_NON);  // NimBLE build of the core's BLE library
#endif
  adv->setMinInterval(ADV_INTERVAL_MS * 1000 / 625);
  adv->setMaxInterval(ADV_INTERVAL_MS * 1000 / 625);
}

// Same advert as the XIAO's non-connectable burst: flags, then the BTHome service data.
static void advertise(const uint8_t *svc, uint8_t len) {
  uint8_t raw[31], n = 0;
  raw[n++] = 2;       raw[n++] = 0x01; raw[n++] = 0x06;          // flags: LE general discoverable, no BR/EDR
  raw[n++] = len + 1; raw[n++] = 0x16;                           // service data; svc starts with the 0xFCD2 UUID
  memcpy(raw + n, svc, len);
  n += len;
  BLEAdvertisementData d;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  d.addData((char *)raw, n);                         // Espressif's "esp32" package 3.x
#else
  d.addData(std::string((char *)raw, n));            // Arduino's "Arduino ESP32 Boards" package (2.0.x)
#endif
  adv->stop();
  adv->setAdvertisementData(d);
  adv->start();
  burstEndsAt = (millis() + ADV_BURST_S * 1000UL) | 1;
}

// Called from the wait loop: ends the burst after ADV_BURST_S, like the XIAO's advertising timeout.
static void serviceBurst() {
  if (burstEndsAt && (int32_t)(millis() - burstEndsAt) >= 0) { adv->stop(); burstEndsAt = 0; }
}
#endif

void setup() {
  Serial.begin(115200);

  pinMode(LED_RED, OUTPUT);   digitalWrite(LED_RED, HIGH);    // LEDs are active-low: off
  pinMode(LED_GREEN, OUTPUT); digitalWrite(LED_GREEN, HIGH);
  pinMode(LED_BLUE, OUTPUT);  digitalWrite(LED_BLUE, HIGH);

#if GW_NRF52
  flashSleep();                                      // the unused QSPI flash chip: about 20 uA saved (v0.13)
#endif
  pinMode(PIN_CAP_POWER, OUTPUT_STRONG);             // the probe pulls a few mA
  digitalWrite(PIN_CAP_POWER, LOW);
  pinMode(PIN_DHT_POWER, OUTPUT_STRONG);             // DHT11 VCC on D3 (v0.13): up to 2.5 mA while measuring
  dht.begin();
  dhtPowerOff();                                     // off until the first reading
  storeBegin();
  loadCal();
#if LIGHT_SENSOR == LIGHT_I2C
  Wire.begin();
  shtOk  = sht4xBegin();
  ahtOk  = !shtOk && aht20Begin();
  vemlOk = veml.begin();
  if (vemlOk) veml.enable(false);                    // shut down until the first reading
  ltrOk  = ltr390Begin();
  airSrc = shtOk ? "sht4x" : ahtOk ? "aht20" : "dht11";
  Serial.printf("I2C sensors on %s: air %s, light %s, UV %s\n", I2C_PINS, airSrc, lightName(), ltrOk ? "ltr390" : "none");
#else
  pinMode(PIN_LDR_POWER, OUTPUT_STRONG);
  digitalWrite(PIN_LDR_POWER, LOW);
#endif

#if GW_NRF52
  analogReference(AR_VDD4);                          // full scale = VDD -> ratiometric readings
  analogReadResolution(12);

  Bluefruit.begin();
  Bluefruit.autoConnLed(false);
  Bluefruit.setTxPower(TX_POWER_DBM);
  Bluefruit.setName("GW");
  wakeSem = xSemaphoreCreateBinary();
  loadKey();
  beginLinkServices();
  bledis.setManufacturer("GROUNDWELL");
  bledis.setModel("Sensor node");
  bledis.setSoftwareRev(FW_VERSION);                 // firmware revision stays the bootloader's (update apps read it)
  bledis.begin();
#else
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);                    // widest input range, about 0 to 3.1 V
  wakeSem = xSemaphoreCreateBinary();
  bleBegin();
#endif

  char mac[18];
  addrText(mac);
  Serial.printf("GROUNDWELL node fw %s  address %s\n", FW_VERSION, mac);
#if GW_NRF52
  Serial.printf("Hub link: %s\n", keyOk ? "linked" : "not linked yet (a hub links it within 10 minutes of a reset)");
  if (DFU_WINDOW_MS) Serial.printf("Bluetooth updates open for %lu min: nRF Connect > \"GW\" > DFU\n", (unsigned long)(DFU_WINDOW_MS / 60000));
#else
  Serial.println("Arduino Nano ESP32 test node: broadcast only. The hub reads it but won't link or update it; flash it over USB.");
#endif
  if (calOk) Serial.printf("Factory calibration: air %u, water %u (sending moisture %%)\n", cal.air, cal.water);
  else       Serial.println("No factory calibration yet (sending the raw capacitive count). Send CAL AIR, then CAL WATER.");
}

void loop() {
  static uint32_t nextAt = 0;
  if (nextAt == 0) nextAt = millis();

  // v0.13: the DHT11 powers up first; soil, supply, light and UV are read during its 1.2 s warm-up.
  bool dhtNow   = dhtInUse();
  if (dhtNow) dhtPowerOn();
  uint16_t cap  = readCapacitive();
  float vdd     = readVDD();
  lastVdd       = vdd;
  float lux = 0;
  bool luxOk    = readLight(lux);
  float uvi = 0;
  bool uvOk     = readUV(uvi);
  float tempC = 0, rh = 0;
  bool airRead  = readAirCycle(tempC, rh);
  readings++;
  if (!airRead) airFails++;
  static AirFilter airF = {false, false, 0, 0, 0, 0};
  bool airOk    = airRead && airFilter(airF, tempC, rh);
  int32_t capPct = calOk ? (int32_t)probeScaleX100(cap) : -1;

  uint8_t svc[31];
  uint8_t len = buildBTHome(svc, ++packetId, airOk, tempC, rh, luxOk, lux, vdd, capPct, cap, uvOk, uvi);
  advertise(svc, len);
  uint32_t now = millis(), dt = lastSentAt ? now - lastSentAt : 0;
  lastSentAt = now;

  if (vdd < 1.6f) Serial.println("WARNING: VDD reading is impossible (the chip needs 1.7 V); please report this line.");
  char airTxt[48], luxTxt[48], capTxt[32], dht1Txt[32] = "";
  if (airOk)        snprintf(airTxt, sizeof airTxt, "air %.1f C %.0f%%RH", tempC, rh);
  else if (airRead) snprintf(airTxt, sizeof airTxt, "air: %.1f C %.0f%%RH held (jump)", tempC, rh);
  else              snprintf(airTxt, sizeof airTxt, "air: %s read failed", airSrc);
  if (dhtNow && DHT_SECOND_MS) {                     // the first read after power-up, to compare (see DHT_SECOND_MS)
    if (dht1Ok) snprintf(dht1Txt, sizeof dht1Txt, " (1st %.1f C %.0f%%)", dht1T, dht1H);
    else        snprintf(dht1Txt, sizeof dht1Txt, " (1st failed)");
  }
#if LIGHT_SENSOR == LIGHT_I2C
  if (luxOk) snprintf(luxTxt, sizeof luxTxt, uvOk ? "light %.0f lux uv %.1f" : "light %.0f lux", lux, uvi);
  else       snprintf(luxTxt, sizeof luxTxt, "light: none");
#else
  snprintf(luxTxt, sizeof luxTxt, "light %.0f lux (adc %u)", lux, ldrAdc);
#endif
  if (capPct >= 0) snprintf(capTxt, sizeof capTxt, "capacitive %4u = %5.1f%%", cap, capPct / 100.0f);
  else             snprintf(capTxt, sizeof capTxt, "capacitive %4u (uncal)", cap);
  Serial.printf("#%u dt=%lums  %s  %s%s  %s  VDD %.2f V\n", packetId, (unsigned long)dt, capTxt, airTxt, dht1Txt, luxTxt, vdd);

  // Absolute schedule: the next reading is due REPORT_MS after the last one was due, however long this
  // one took. The wait is a loop on the clock, so it can't end early, and it answers serial commands.
  uint32_t every = (fastUntil && (int32_t)(fastUntil - millis()) > 0) ? FAST_REPORT_MS : REPORT_MS;
  nextAt += every;
  if ((int32_t)(millis() - nextAt) >= 0) nextAt = millis() + every;       // fell behind (a 10 s calibration, say)
  while ((int32_t)(nextAt - millis()) > 0) {
    serviceSerial();
    if (fastKick) {                                  // FAST (v0.13): next reading within 5 s, not up to a minute
      fastKick = false;
      uint32_t soon = millis() + FAST_REPORT_MS;
      if ((int32_t)(nextAt - soon) > 0) nextAt = soon;
    }
#if GW_NRF52
    serviceLink();
#else
    serviceBurst();
#endif
    uint32_t left = nextAt - millis();
    if ((int32_t)left <= 0) break;
#if GW_NRF52
    uint32_t wait = Serial ? (left > 50 ? 50 : left) : left;
#else
    uint32_t wait = left > 50 ? 50 : left;           // also ends the 6 s burst on time
#endif
    xSemaphoreTake(wakeSem, pdMS_TO_TICKS(wait));    // the CPU sleeps here; a link-key write wakes it early
  }
}

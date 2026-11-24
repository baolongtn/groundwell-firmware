// Host-side test of the node's sensor math: CRC, SHT4x / AHT20 conversions, LTR390 scaling, air glitch filter.
// Run from the tools folder:
//   python3 -c "s=open('../node/GroundwellNode/GroundwellNode.ino').read(); a=s.index('// ---- sensor math'); b=s.index('// ---- end sensor math'); open('node_math.inc','w').write(s[a:b])"
//   g++ -std=c++17 -Wall -o sensormath sensor_math_test.cpp && ./sensormath
#include <cstdio>
#include <cstdint>
#include <cmath>
static const float AIR_JUMP_C = 3.0f, AIR_JUMP_RH = 15.0f, AIR_CONFIRM_C = 1.5f, AIR_CONFIRM_RH = 6.0f;
struct AirFilter { bool have, pend; float t, h, pt, ph; };   // same as the node sketch (declared with its globals)
#include "node_math.inc"

int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
#define NEAR(a, b, tol) (fabsf((a) - (b)) <= (tol))

int main() {
  // CRC-8 worked example from the Sensirion SHT4x datasheet: 0xBEEF -> 0x92
  uint8_t ex[2] = {0xBE, 0xEF};
  CHECK(crc8(ex, 2) == 0x92);

  // SHT4x: 25.00 C, 50.0 %RH
  uint16_t st = (uint16_t)lroundf((25.0f + 45.0f) / 175.0f * 65535.0f), srh = (uint16_t)lroundf((50.0f + 6.0f) / 125.0f * 65535.0f);
  uint8_t f[6] = {(uint8_t)(st >> 8), (uint8_t)st, 0, (uint8_t)(srh >> 8), (uint8_t)srh, 0};
  f[2] = crc8(f, 2); f[5] = crc8(f + 3, 2);
  float t, h;
  CHECK(sht4xConvert(f, t, h) && NEAR(t, 25.0f, 0.01f) && NEAR(h, 50.0f, 0.01f));
  f[4] ^= 1;  CHECK(!sht4xConvert(f, t, h));                      // corrupted byte -> rejected
  uint8_t lo[6] = {0x00, 0x00, 0, 0x00, 0x00, 0}; lo[2] = crc8(lo, 2); lo[5] = crc8(lo + 3, 2);
  CHECK(sht4xConvert(lo, t, h) && NEAR(t, -45.0f, 0.01f) && h == 0.0f);   // RH clamped at 0

  // AHT20: 50 %RH (0x80000), 25 C (0x60000)
  uint32_t ar = 0x80000, at = 0x60000;
  uint8_t a[7] = {0x1C, (uint8_t)(ar >> 12), (uint8_t)(ar >> 4), (uint8_t)(((ar & 0xF) << 4) | (at >> 16)), (uint8_t)(at >> 8), (uint8_t)at, 0};
  a[6] = crc8(a, 6);
  CHECK(aht20Convert(a, t, h) && NEAR(t, 25.0f, 0.01f) && NEAR(h, 50.0f, 0.01f));
  a[0] |= 0x80; a[6] = crc8(a, 6); CHECK(!aht20Convert(a, t, h)); // busy -> rejected
  a[0] = 0x1C;  a[6] ^= 0xFF;      CHECK(!aht20Convert(a, t, h)); // bad CRC -> rejected

  // LTR390: 2300 counts at 18x/20-bit = UV index 1; ambient 1000 counts at 3x/18-bit = 200 lux
  CHECK(NEAR(ltrUvIndex(2300), 1.0f, 1e-4f));
  CHECK(NEAR(ltrLux(1000, 1), 200.0f, 1e-3f) && NEAR(ltrLux(1000, 0), 600.0f, 1e-3f));

  // Air filter
  AirFilter af = {false, false, 0, 0, 0, 0};
  CHECK(airFilter(af, 22.0f, 45.0f));                             // first reading accepted
  CHECK(airFilter(af, 22.4f, 47.0f));                             // small change accepted
  CHECK(!airFilter(af, 0.3f, 45.0f));                             // one-off glitch held back
  CHECK(airFilter(af, 22.5f, 46.0f));                             // back to normal: accepted, glitch dropped
  CHECK(!airFilter(af, 22.5f, 85.0f));                            // breath on it: held once ...
  CHECK(airFilter(af, 22.9f, 82.0f));                             // ... confirmed by the next reading
  CHECK(af.t == 22.9f && af.h == 82.0f);
  CHECK(!airFilter(af, 31.0f, 20.0f) && !airFilter(af, 10.0f, 60.0f));  // garbage that never repeats stays out

  printf(fails ? "%d FAILURES\n" : "ALL PASS\n", fails);
  return fails ? 1 : 0;
}

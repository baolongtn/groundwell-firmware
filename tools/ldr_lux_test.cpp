// Host-side test of the node's photoresistor conversion (LIGHT_LDR).
// Run from the tools folder:
//   python3 -c "s=open('../node/GroundwellNode/GroundwellNode.ino').read(); a=s.index('// Photoresistor model'); b=s.index('static const uint16_t LDR_SETTLE_MS'); c=s.index('__attribute__((unused)) static float ldrLux('); d=s.index('#if LIGHT_SENSOR == LIGHT_I2C', c); open('node_ldr.inc','w').write(s[a:b]+s[c:d])"
//   g++ -std=c++17 -Wall -o ldrtest ldr_lux_test.cpp && ./ldrtest
#include <cstdio>
#include <cmath>
#include <cstdint>
#include "node_ldr.inc"   // generated: LDR constants + ldrLux() copied from the node sketch

int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

// adc the divider would give for a given lux, from the same model
static uint16_t adcFor(float lux) {
  float r = LDR_R10_OHMS * powf(lux / 10.0f, -LDR_GAMMA);
  return (uint16_t)lroundf(4095.0f * LDR_FIXED_OHMS / (r + LDR_FIXED_OHMS));
}

int main() {
  // Round trip across the range a houseplant sees: dim corner to direct sun
  const float pts[] = {10, 50, 250, 1000, 5000, 20000, 50000, 100000};
  for (float lx : pts) {
    uint16_t a = adcFor(lx);
    float back = ldrLux(a);
    float err = fabsf(back - lx) / lx;
    printf("%8.0f lux -> adc %4u -> %8.0f lux  (%.1f%% quantization)\n", lx, a, back, err * 100);
    CHECK(err < (lx < 30 ? 0.10f : 0.03f));   // 12-bit steps: coarse only at the very dark end
  }
  // Monotonic: more counts = more light
  float prev = -1; bool mono = true;
  for (int a = 1; a <= 4094; a++) { float v = ldrLux((uint16_t)a); if (!(v > prev)) mono = false; prev = v; }
  CHECK(mono);
  CHECK(ldrLux(0) == 0.0f);                    // dark or unplugged
  CHECK(std::isfinite(ldrLux(4095)));          // saturation is clamped, not infinite
  CHECK(ldrLux(4095) > 100000.0f);
  // R at exactly R10 reads 10 lux
  uint16_t a10 = (uint16_t)lroundf(4095.0f * LDR_FIXED_OHMS / (LDR_R10_OHMS + LDR_FIXED_OHMS));
  CHECK(fabsf(ldrLux(a10) - 10.0f) < 1.5f);
  printf(fails ? "%d FAILED\n" : "ALL PASS\n", fails);
  return fails ? 1 : 0;
}

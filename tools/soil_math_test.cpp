// Host-side test of the EVT1 soil math (v0.13): EC divider to soil resistance, conductivity, NTC temperature,
// and the CAL EC cell constant. The functions are cut from the node sketch, between its soil math markers.
// Run from the tools folder:
//   python3 -c "s=open('../node/GroundwellNode/GroundwellNode.ino').read(); a=s.index('// ---- soil math'); b=s.index('// ---- end soil math'); open('node_soil.inc','w').write(s[a:b])"
//   g++ -std=c++17 -Wall -o soilmath soil_math_test.cpp && ./soilmath
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <initializer_list>
// the node's EVT1 settings (same values as the sketch)
static const float EC_R_REF = 10000.0f, EC_R_SERIES = 2000.0f, EC_ALPHA = 0.02f;
static const float NTC_R_TOP = 10000.0f, NTC_R25 = 10000.0f, NTC_BETA = 3380.0f;
#include "node_soil.inc"

int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
#define NEAR(a, b, tol) (fabsf((a) - (b)) <= (tol))

// What the ADC reads in each direction for a given soil resistance
static void divider(float soil, float &a, float &b) {
  float rx = soil + EC_R_SERIES;
  a = 4095.0f * rx / (EC_R_REF + rx);
  b = 4095.0f * EC_R_REF / (EC_R_REF + rx);
}

int main() {
  float a, b;
  for (float soil : {300.0f, 1000.0f, 5000.0f, 20000.0f, 100000.0f}) {
    divider(soil, a, b);
    float r = ecSoilOhms(a, b);
    CHECK(NEAR(r, soil, soil * 0.002f + 1.0f));
    printf("soil %.0f ohm -> a %.0f b %.0f -> %.0f ohm -> %u uS/cm at k=1\n", soil, a, b, r, ecMicroSiemens(r, 1.0f));
  }
  divider(5000.0f, a, b);
  CHECK(NEAR(ecSoilOhms(a, 0.0f), 5000.0f, 15.0f));             // one direction lost: the other still gives the answer
  CHECK(NEAR(ecSoilOhms(4095.0f, b), 5000.0f, 15.0f));
  CHECK(ecSoilOhms(4095.0f, 0.0f) < 0);                          // open circuit: dry soil or the stake is out
  divider(0.0f, a, b);
  CHECK(ecSoilOhms(a, b) == 1.0f);                               // shorted pads clamp to 1 ohm, never zero or negative

  CHECK(ecMicroSiemens(5000.0f, 1.0f) == 200);
  CHECK(ecMicroSiemens(5000.0f, 0.5f) == 100);
  CHECK(ecMicroSiemens(-1.0f, 1.0f) == 0);
  CHECK(ecMicroSiemens(1.0f, 20.0f) == 65535);                   // clamps to the BTHome u16

  // NTC: the beta equation turned around should give back the temperature
  for (float t : {0.0f, 10.0f, 25.0f, 40.0f}) {
    float r = NTC_R25 * expf(NTC_BETA * (1.0f / (t + 273.15f) - 1.0f / 298.15f));
    float counts = 4095.0f * r / (NTC_R_TOP + r), got = -99;
    CHECK(ntcCelsius(counts, got) && NEAR(got, t, 0.05f));
  }
  float got;
  CHECK(!ntcCelsius(4095.0f, got));                              // NTC open
  CHECK(!ntcCelsius(0.0f, got));                                 // NTC shorted

  // CAL EC: a 1413 uS/cm standard at 25 C through 708 ohms gives k of about 1.0; the same reading then reads 1413
  float k = ecCellConstant(708.0f, 1413.0f, 25.0f);
  CHECK(NEAR(k, 1.0004f, 0.0005f));
  CHECK(ecMicroSiemens(708.0f, k) == 1413);
  // At 20 C the standard really is about 10% lower, so the same resistance means a smaller cell constant
  float k20 = ecCellConstant(708.0f, 1413.0f, 20.0f);
  CHECK(NEAR(k20, 0.9004f, 0.0005f));
  CHECK(NEAR(ecMicroSiemens(708.0f, k20) / (1.0f + EC_ALPHA * (20.0f - 25.0f)), 1413.0f, 2.0f));

  printf(fails ? "%d FAILURES\n" : "ALL PASS\n", fails);
  return fails ? 1 : 0;
}

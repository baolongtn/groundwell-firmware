// Host-side test: node packet builder -> hub decoder.
// Run from the tools folder:
//   python3 -c "s=open('../node/GroundwellNode/GroundwellNode.ino').read(); a=s.index('static uint8_t buildBTHome('); b=s.index('// ---- end BTHome packet'); open('node_build.inc','w').write(s[a:b])"
//   g++ -std=c++17 -Wall -o roundtrip bthome_roundtrip_test.cpp && ./roundtrip
#include <cstdio>
#include <cmath>
#include <cstdint>
#include "../hub/GroundwellHub/bthome.h"
#include "node_build.inc"   // generated: buildBTHome() copied from the node sketch

int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

int main() {
  // capPct: probe scale x100 for a factory-calibrated node, -1 for an uncalibrated one (raw count instead)
  struct Case { uint8_t pid; bool air; float t, rh; bool lx; float lux, v; int32_t capPct; uint16_t cap, rod; bool uvOk; float uv; } cases[] = {
    {1, true, 24.75f, 48.0f, true, 8423.57f, 3.301f, -1, 2873, 412, false, 0},
    {255, true, -5.25f, 99.5f, true, 120000.0f, 2.201f, -1, 0, 4095, true, 11.3f},  // direct sun, UV 11.3
    {9, true, 21.0f, 40.0f, true, 0.0f, 3.1f, -1, 2000, 900, true, 0.0f},             // dark
    {17, false, 0, 0, true, 3.21f, 3.0f, -1, 1500, 2800, false, 0},                    // air read failed this cycle
    {18, true, 22.0f, 55.0f, false, 0, 3.0f, -1, 1500, 2800, false, 0},                // no light reading
    {19, false, 0, 0, false, 0, 3.0f, -1, 1500, 2800, false, 0},                       // neither
    {20, true, 22.0f, 55.0f, true, 5000.0f, 3.3f, 4567, 2283, 300, true, 0.4f},        // factory calibrated: 45.67%
    {21, true, 22.0f, 55.0f, true, 5000.0f, 3.3f, 0, 3200, 0, false, 0},               // calibrated, in air: 0%
    {22, false, 0, 0, false, 0, 3.3f, 10000, 1400, 2000, true, 30.0f},                 // calibrated, in water: 100%; UV clamps
  };
  for (auto &c : cases) {
    uint8_t b[32];
    uint8_t n = buildBTHome(b, c.pid, c.air, c.t, c.rh, c.lx, c.lux, c.v, c.capPct, c.cap, c.rod, c.uvOk, c.uv);
    CHECK(n == 14 + (c.air ? 6 : 0) + (c.lx ? 4 : 0) + (c.uvOk ? 2 : 0));   // calibrated or not, the same size
    CHECK(b[0] == 0xD2 && b[1] == 0xFC && b[2] == 0x40);
    CHECK(3 + 2 + n <= 31);                               // flags AD + service-data AD fit the legacy advert
    printf("packet:");
    for (int i = 0; i < n; i++) printf(" %02X", b[i]);
    printf("\n");
    BTHomePacket p;                                       // ESP32 library strips the 2-byte UUID
    CHECK(parseBTHome(b + 2, n - 2, p));
    CHECK(p.hasPid && p.pid == c.pid);
    CHECK(!p.hasBatt);                                     // v0.9: battery level comes from the voltage
    CHECK(p.hasUV == c.uvOk);
    if (c.uvOk) CHECK(fabsf(p.uv - (c.uv > 25.5f ? 25.5f : c.uv)) < 0.051f);
    CHECK(p.hasVolt && fabsf(p.volt - c.v) < 0.0006f);
    CHECK(p.hasTemp == c.air && p.hasHum == c.air);
    if (c.air) { CHECK(fabsf(p.tempC - c.t) < 0.006f); CHECK(fabsf(p.humidity - c.rh) < 0.006f); }
    CHECK(p.hasLux == c.lx);
    if (c.lx) CHECK(fabsf(p.lux - c.lux) < 0.006f + c.lux * 1e-6f);
    if (c.capPct >= 0) {
      CHECK(p.hasMoist && fabsf(p.moist - c.capPct / 100.0f) < 0.006f);
      CHECK(p.nCounts == 1 && p.counts[0] == c.rod);
    } else {
      CHECK(!p.hasMoist);
      CHECK(p.nCounts == 2 && p.counts[0] == c.cap && p.counts[1] == c.rod);
    }
  }
  BTHomePacket q;
  uint8_t enc[] = {0x41, 0x00, 0x01};               CHECK(!parseBTHome(enc, sizeof enc, q));   // encrypted
  uint8_t v1[]  = {0x20, 0x00, 0x01};               CHECK(!parseBTHome(v1, sizeof v1, q));     // BTHome v1
  uint8_t cut[] = {0x40, 0x00, 0x05, 0x3D, 0x10};   CHECK(!parseBTHome(cut, sizeof cut, q));   // truncated
  uint8_t lux3[] = {0x40, 0x05, 0x10, 0x27};          CHECK(!parseBTHome(lux3, sizeof lux3, q));  // illuminance cut short
  uint8_t unk[] = {0x40, 0x00, 0x07, 0x3D, 0x34, 0x12, 0xF0, 0x01};
  CHECK(parseBTHome(unk, sizeof unk, q) && q.nCounts == 1 && q.counts[0] == 0x1234);           // unknown id after data
  uint8_t moi[] = {0x40, 0x00, 0x03, 0x14, 0xD7, 0x11};   // moisture only: 45.67%
  CHECK(parseBTHome(moi, sizeof moi, q) && q.hasMoist && fabsf(q.moist - 45.67f) < 0.006f && q.nCounts == 0);
  uint8_t old[] = {0x40, 0x00, 0x05, 0x01, 0x64, 0x0C, 0xE4, 0x0C, 0x3D, 0x34, 0x12};   // v0.6 node: battery % still parses
  CHECK(parseBTHome(old, sizeof old, q) && q.hasBatt && q.batt == 100 && q.nCounts == 1);
  printf(fails ? "%d FAILURES\n" : "ALL PASS\n", fails);
  return fails ? 1 : 0;
}

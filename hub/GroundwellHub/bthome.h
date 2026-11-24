// BTHome v2 decoder (unencrypted). Shared by the hub and the host-side test.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

struct BTHomePacket {
  bool     hasPid;
  uint8_t  pid;
  bool     hasBatt;
  uint8_t  batt;        // %
  bool     hasTemp;
  float    tempC;
  bool     hasVolt;
  float    volt;        // V
  bool     hasHum;
  float    humidity;    // %RH
  bool     hasLux;
  float    lux;         // illuminance, lux
  bool     hasUV;
  float    uv;          // UV index, 0.1 steps (v0.9 nodes with an LTR390)
  bool     hasMoist;
  float    moist;       // moisture %, 0.01 steps (GROUNDWELL: factory-calibrated capacitive probe, 0 = air, 100 = water)
  uint8_t  nCounts;
  uint32_t counts[4];   // probe readings, in packet order
};

// `d` starts at the BTHome device-info byte (the 0xFCD2 UUID already stripped).
static inline bool parseBTHome(const uint8_t *d, size_t n, BTHomePacket &p) {
  memset(&p, 0, sizeof(p));
  if (n < 1) return false;
  uint8_t info = d[0];
  if (info & 0x01) return false;               // encrypted: not supported here
  if ((info >> 5) != 2) return false;          // BTHome v2 only
  size_t i = 1;
  while (i < n) {
    uint8_t id = d[i++];
    size_t need;
    switch (id) {
      case 0x00: case 0x01: case 0x09: case 0x2E: case 0x2F: case 0x46: need = 1; break;
      case 0x02: case 0x03: case 0x0C: case 0x14: case 0x3D: case 0x08: need = 2; break;
      case 0x04: case 0x05: case 0x0A: case 0x0B: need = 3; break;
      case 0x3E: need = 4; break;
      default: return p.nCounts > 0 || p.hasMoist || p.hasPid; // unknown object: stop, keep what we have
    }
    if (i + need > n) return false;
    const uint8_t *v = d + i;
    switch (id) {
      case 0x00: p.hasPid = true;  p.pid = v[0]; break;
      case 0x01: p.hasBatt = true; p.batt = v[0]; break;
      case 0x02: p.hasTemp = true; p.tempC = (int16_t)(v[0] | (v[1] << 8)) / 100.0f; break;
      case 0x0C: p.hasVolt = true; p.volt = (uint16_t)(v[0] | (v[1] << 8)) / 1000.0f; break;
      case 0x03: p.hasHum = true;  p.humidity = (uint16_t)(v[0] | (v[1] << 8)) / 100.0f; break;
      case 0x2E: p.hasHum = true;  p.humidity = v[0]; break;
      case 0x05: p.hasLux = true;  p.lux = ((uint32_t)v[0] | ((uint32_t)v[1] << 8) | ((uint32_t)v[2] << 16)) / 100.0f; break;
      case 0x14: p.hasMoist = true; p.moist = (uint16_t)(v[0] | (v[1] << 8)) / 100.0f; break;
      case 0x46: p.hasUV = true;    p.uv = v[0] / 10.0f; break;
      case 0x09: if (p.nCounts < 4) p.counts[p.nCounts++] = v[0]; break;
      case 0x3D: if (p.nCounts < 4) p.counts[p.nCounts++] = (uint16_t)(v[0] | (v[1] << 8)); break;
      case 0x3E: if (p.nCounts < 4) p.counts[p.nCounts++] = (uint32_t)v[0] | ((uint32_t)v[1] << 8) |
                                                           ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 24); break;
      default: break;                            // known size, not used
    }
    i += need;
  }
  return true;
}

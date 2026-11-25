#pragma once
// Web update check (v0.11.1): is the uploaded file a hub app image for this chip?
//
// The web server hands an upload over in 1436-byte pieces, but the ESP32 Update library only advances
// Update.progress() once it has a full 4 KB sector to write. Up to v0.11.0 the hub used
// "Update.progress() == 0" to mean "this is the first piece", so it checked the second piece as if it were
// the start of the file and refused every real upload. This check keeps its own state instead: it collects
// the first 36 bytes of the file, however they arrive, and decides once.
//
// Plain C++ with no Arduino dependencies, so tools/ota_check_test.cpp runs the same code on the host.

#include <stddef.h>
#include <stdint.h>

enum GwImgVerdict : uint8_t { GW_IMG_NEED_MORE = 0, GW_IMG_OK = 1, GW_IMG_BAD = 2 };

struct GwImgCheck {
  uint8_t head[36];   // ESP image header (24) + first segment header (8) + esp_app_desc_t magic (4)
  uint8_t have;
  uint8_t verdict;
};

static const uint16_t GW_IMG_ANY_CHIP = 0xFFFF;

static inline void gwImgReset(GwImgCheck &c) { c.have = 0; c.verdict = GW_IMG_NEED_MORE; }

// Feed each piece of the upload in order. Returns the verdict so far; once OK or BAD it stays that way.
static inline uint8_t gwImgFeed(GwImgCheck &c, const uint8_t *p, size_t n, uint16_t chipId) {
  if (c.verdict != GW_IMG_NEED_MORE) return c.verdict;
  while (n && c.have < sizeof c.head) { c.head[c.have++] = *p++; n--; }
  if (c.have < sizeof c.head) return c.verdict;
  const uint8_t *b = c.head;
  bool image = b[0] == 0xE9;                                                       // ESP image magic
  bool app   = b[32] == 0x32 && b[33] == 0x54 && b[34] == 0xCD && b[35] == 0xAB;   // esp_app_desc_t magic
  bool chip  = chipId == GW_IMG_ANY_CHIP || (uint16_t)(b[12] | (b[13] << 8)) == chipId;
  c.verdict = (image && app && chip) ? GW_IMG_OK : GW_IMG_BAD;
  return c.verdict;
}

// At the end of the upload: a file shorter than the header never got a verdict, and isn't an image.
static inline bool gwImgAccepted(const GwImgCheck &c) { return c.verdict == GW_IMG_OK; }

// Legacy Nordic DFU, client side: the hub sends a node firmware to the node's bootloader
// (Adafruit nRF52 bootloader, SDK 11 "legacy" DFU service 00001530-1212-EFDE-1523-785FEABCD123).
// No Bluetooth code in here: the hub plugs in a DfuLink that talks to the real node, and
// tools/node_dfu_test.cpp plugs in a simulated bootloader.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

struct DfuLink {
  virtual bool writeCtrl(const uint8_t *d, size_t n) = 0;             // control point, write with response
  virtual bool writePkt(const uint8_t *d, size_t n) = 0;              // packet, write without response
  virtual bool waitNotify(uint8_t *out, size_t &n, uint32_t ms) = 0;  // next control-point notification
  virtual void drain() {}                                             // forget queued notifications
  virtual void progress(uint32_t done, uint32_t total) { (void)done; (void)total; }
  virtual ~DfuLink() {}
};

enum : uint8_t { DFU_OP_START = 1, DFU_OP_INIT = 2, DFU_OP_RECEIVE = 3, DFU_OP_VALIDATE = 4, DFU_OP_ACTIVATE = 5,
                 DFU_OP_RESET = 6, DFU_OP_PRN_REQ = 8, DFU_OP_RESPONSE = 0x10, DFU_OP_PRN = 0x11 };
static const uint8_t DFU_IMAGE_APP = 4;

static inline void dfuPut32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static inline uint32_t dfuGet32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static const char *dfuStepName(uint8_t op) {
  switch (op) { case DFU_OP_START: return "start"; case DFU_OP_INIT: return "init packet"; case DFU_OP_RECEIVE: return "transfer";
                case DFU_OP_VALIDATE: return "check"; default: return "step"; }
}

static const char *dfuStatusText(uint8_t s) {
  switch (s) {
    case 2: return "the bootloader was in the wrong state";
    case 3: return "the bootloader doesn't support this";
    case 4: return "the firmware is too big for the node";
    case 5: return "the firmware failed its checksum";
    case 6: return "the bootloader reported an error";
    default: return "the bootloader answered with an unknown code";
  }
}

// Waits for the response to one step, skipping late packet receipts. nullptr = success.
static const char *dfuExpect(DfuLink &l, uint8_t op, uint32_t ms, char *err, size_t errLen) {
  for (int skipped = 0; skipped < 4; skipped++) {
    uint8_t m[20]; size_t n = 0;
    if (!l.waitNotify(m, n, ms)) { snprintf(err, errLen, "no answer to the %s step", dfuStepName(op)); return err; }
    if (n >= 1 && m[0] == DFU_OP_PRN) continue;
    if (n >= 3 && m[0] == DFU_OP_RESPONSE && m[1] == op) {
      if (m[2] == 1) return nullptr;
      snprintf(err, errLen, "%s step: %s", dfuStepName(op), dfuStatusText(m[2]));
      return err;
    }
    snprintf(err, errLen, "unexpected answer to the %s step", dfuStepName(op));
    return err;
  }
  snprintf(err, errLen, "no answer to the %s step", dfuStepName(op));
  return err;
}

// Sends one application image. pkt = bytes per data packet (multiple of 4, at most MTU - 3),
// prn = packets between receipts from the node. Returns nullptr on success, else the reason.
// On success the node has been told to activate the new firmware and restart.
static const char *dfuTransfer(DfuLink &l, const uint8_t *bin, uint32_t len, const uint8_t *dat, uint16_t datLen,
                               uint16_t pkt, uint16_t prn, char *err, size_t errLen) {
  if (len == 0 || (len & 3)) { snprintf(err, errLen, "image size %lu isn't a multiple of 4", (unsigned long)len); return err; }
  if (pkt < 4 || (pkt & 3)) { snprintf(err, errLen, "bad packet size %u", (unsigned)pkt); return err; }
  if (prn == 0) prn = 8;
  l.drain();
  uint8_t c[3];
  const char *e;

  c[0] = DFU_OP_START; c[1] = DFU_IMAGE_APP;
  if (!l.writeCtrl(c, 2)) { snprintf(err, errLen, "the node refused the start command"); return err; }
  uint8_t sizes[12] = {0};
  dfuPut32(sizes + 8, len);                                   // softdevice 0, bootloader 0, application len
  if (!l.writePkt(sizes, sizeof sizes)) { snprintf(err, errLen, "lost the connection sending the size"); return err; }
  if ((e = dfuExpect(l, DFU_OP_START, 30000, err, errLen))) return e;   // the node erases flash first: a few seconds

  c[0] = DFU_OP_INIT; c[1] = 0;
  if (!l.writeCtrl(c, 2)) { snprintf(err, errLen, "the node refused the init command"); return err; }
  for (uint16_t off = 0; off < datLen; off += 20) {
    uint16_t n = datLen - off < 20 ? datLen - off : 20;
    if (!l.writePkt(dat + off, n)) { snprintf(err, errLen, "lost the connection sending the init packet"); return err; }
  }
  c[1] = 1;
  if (!l.writeCtrl(c, 2)) { snprintf(err, errLen, "the node refused the init command"); return err; }
  if ((e = dfuExpect(l, DFU_OP_INIT, 10000, err, errLen))) return e;

  c[0] = DFU_OP_PRN_REQ; c[1] = prn & 0xFF; c[2] = prn >> 8;
  if (!l.writeCtrl(c, 3)) { snprintf(err, errLen, "the node refused the receipt setting"); return err; }
  c[0] = DFU_OP_RECEIVE;
  if (!l.writeCtrl(c, 1)) { snprintf(err, errLen, "the node refused the transfer command"); return err; }

  uint32_t sent = 0;
  uint16_t since = 0;
  while (sent < len) {
    uint16_t n = len - sent < pkt ? (uint16_t)(len - sent) : pkt;
    if (!l.writePkt(bin + sent, n)) { snprintf(err, errLen, "lost the connection at %lu of %lu bytes", (unsigned long)sent, (unsigned long)len); return err; }
    sent += n;
    l.progress(sent, len);
    if (++since == prn && sent < len) {                       // after the last packet the node answers the step instead
      since = 0;
      uint8_t m[20]; size_t mn = 0;
      if (!l.waitNotify(m, mn, 5000)) { snprintf(err, errLen, "no receipt from the node at %lu bytes", (unsigned long)sent); return err; }
      if (mn >= 3 && m[0] == DFU_OP_RESPONSE) { snprintf(err, errLen, "transfer: %s", dfuStatusText(m[2])); return err; }
      if (mn < 5 || m[0] != DFU_OP_PRN) { snprintf(err, errLen, "unexpected message during the transfer"); return err; }
      uint32_t got = dfuGet32(m + 1);
      if (got != sent) { snprintf(err, errLen, "the node got %lu bytes, %lu were sent", (unsigned long)got, (unsigned long)sent); return err; }
    }
  }
  if ((e = dfuExpect(l, DFU_OP_RECEIVE, 15000, err, errLen))) return e;

  c[0] = DFU_OP_VALIDATE;
  if (!l.writeCtrl(c, 1)) { snprintf(err, errLen, "the node refused the check command"); return err; }
  if ((e = dfuExpect(l, DFU_OP_VALIDATE, 15000, err, errLen))) return e;

  c[0] = DFU_OP_ACTIVATE;
  l.writeCtrl(c, 1);                                          // the node restarts at once; the reply may never come
  return nullptr;
}

// "0.10.0" > "0.9.1": compares dotted numbers left to right. Text after the numbers is ignored.
// v0.13: a node build for another board carries a suffix ("0.13.0-evt1"). The hub updates a node only with a build
// for the same board: same suffix after the numbers (none for the breadboard node), compared up to any ';'.
static bool dfuSameBoard(const char *bundled, const char *node) {
  while (*bundled && (*bundled == '.' || (*bundled >= '0' && *bundled <= '9'))) bundled++;
  while (*node && (*node == '.' || (*node >= '0' && *node <= '9'))) node++;
  while (*bundled && *bundled != ';' && *node && *node != ';') if (*bundled++ != *node++) return false;
  return (!*bundled || *bundled == ';') && (!*node || *node == ';');
}

static int dfuVersionCompare(const char *a, const char *b) {
  for (int i = 0; i < 4; i++) {
    long x = 0, y = 0;
    while (*a >= '0' && *a <= '9') x = x * 10 + (*a++ - '0');
    while (*b >= '0' && *b <= '9') y = y * 10 + (*b++ - '0');
    if (x != y) return x < y ? -1 : 1;
    if (*a == '.') a++; else a = "";
    if (*b == '.') b++; else b = "";
    if (!*a && !*b) return 0;
  }
  return 0;
}

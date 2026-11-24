// Runs the hub's node-update client (hub/GroundwellHub/node_dfu.h) against a simulated bootloader that
// follows the Adafruit nRF52 bootloader's legacy DFU rules (lib/sdk11 dfu_transport_ble.c / ble_dfu.c):
// 12-byte size packet after START, init packet, receipts every N data packets, data packets a multiple
// of 4 bytes and no longer than the packet characteristic allows, CRC16 check of the image.
// Build and run:  g++ -O2 -Wall -o node_dfu_test node_dfu_test.cpp && ./node_dfu_test
#include <cstdio>
#include <deque>
#include <vector>
#include <string>
#include "../hub/GroundwellHub/node_dfu.h"

static uint16_t crc16ccitt(const uint8_t *p, size_t n) {   // the bootloader's crc16_compute()
  uint16_t crc = 0xFFFF;
  while (n--) { crc = (uint8_t)(crc >> 8) | (crc << 8); crc ^= *p++; crc ^= (uint8_t)(crc & 0xFF) >> 4; crc ^= (crc << 8) << 4; crc ^= ((crc & 0xFF) << 4) << 1; }
  return crc;
}

struct SimBootloader : DfuLink {
  // knobs
  size_t maxPkt = 244;            // packet characteristic max length (old bootloaders: 20)
  uint32_t maxApp = 400000;
  int dropReceiptNo = -1;         // drop this receipt (0-based), as if the notification were lost
  bool lateReceiptAtEnd = true;   // also send a receipt when the last packet completes a group
  // state
  enum { IDLE, START, INIT, READY, RECV, DONE } st = IDLE;
  std::deque<std::vector<uint8_t>> q;
  std::vector<uint8_t> img, dat;
  uint32_t appLen = 0;
  uint16_t prn = 0, cnt = 0;
  int receipts = 0;
  bool activated = false, validated = false;
  uint32_t lastProgress = 0;

  void notify(std::vector<uint8_t> m) { q.push_back(m); }
  bool writeCtrl(const uint8_t *d, size_t n) override {
    switch (d[0]) {
      case DFU_OP_START: if (st != IDLE) { notify({0x10, 1, 2}); return true; } st = START; return true;
      case DFU_OP_INIT:
        if (n < 2) return false;
        if (d[1] == 0) { st = INIT; dat.clear(); return true; }
        if (st != INIT) { notify({0x10, 2, 2}); return true; }
        notify({0x10, 2, (uint8_t)(dat.size() >= 2 ? 1 : 6)}); st = READY; return true;
      case DFU_OP_PRN_REQ: prn = d[1] | d[2] << 8; cnt = 0; return true;
      case DFU_OP_RECEIVE: if (st != READY) { notify({0x10, 3, 2}); return true; } st = RECV; img.clear(); return true;
      case DFU_OP_VALIDATE: {
        if (st != DONE) { notify({0x10, 4, 2}); return true; }
        uint16_t want = dat.size() >= 2 ? (dat[dat.size() - 2] | dat[dat.size() - 1] << 8) : 0;
        bool ok = crc16ccitt(img.data(), img.size()) == want;
        validated = ok;
        notify({0x10, 4, (uint8_t)(ok ? 1 : 5)});
        return true;
      }
      case DFU_OP_ACTIVATE: activated = validated; return true;
      default: return true;
    }
  }
  bool writePkt(const uint8_t *d, size_t n) override {
    if (n > maxPkt) return true;                       // SoftDevice drops an over-long write without a word
    if (st == START) {
      if (n != 12) { notify({0x10, 1, 3}); return true; }
      appLen = dfuGet32(d + 8);
      bool ok = dfuGet32(d) == 0 && dfuGet32(d + 4) == 0 && appLen && appLen <= maxApp;
      notify({0x10, 1, (uint8_t)(ok ? 1 : 4)});
      st = ok ? IDLE : IDLE;                           // stays idle until INIT starts
      if (ok) st = IDLE;
      return true;
    }
    if (st == INIT) { dat.insert(dat.end(), d, d + n); return true; }
    if (st == RECV) {
      if (n & 3) { notify({0x10, 3, 3}); return true; }
      img.insert(img.end(), d, d + n);
      bool complete = img.size() >= appLen;
      if (prn && ++cnt == prn) {
        cnt = 0;
        if (!complete || lateReceiptAtEnd) {
          uint8_t m[5] = {0x11}; dfuPut32(m + 1, (uint32_t)img.size());
          if (receipts++ != dropReceiptNo) notify(std::vector<uint8_t>(m, m + 5));
        }
      }
      if (complete) { st = DONE; notify({0x10, 3, (uint8_t)(img.size() == appLen ? 1 : 4)}); }
      return true;
    }
    return true;
  }
  bool waitNotify(uint8_t *out, size_t &n, uint32_t) override {
    if (q.empty()) return false;                       // instant "timeout"
    n = q.front().size(); memcpy(out, q.front().data(), n); q.pop_front(); return true;
  }
  void drain() override { q.clear(); }
  void progress(uint32_t done, uint32_t) override { lastProgress = done; }
};

static std::vector<uint8_t> makeImage(size_t n) { std::vector<uint8_t> v(n); for (size_t i = 0; i < n; i++) v[i] = (uint8_t)(i * 31 + (i >> 7)); return v; }
static std::vector<uint8_t> makeDat(const std::vector<uint8_t> &img) {   // adafruit-nrfutil init packet: header fields + CRC16 last
  std::vector<uint8_t> d = {0x52, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0x00, 0x23, 0x01};
  uint16_t c = crc16ccitt(img.data(), img.size()); d.push_back(c & 0xFF); d.push_back(c >> 8); return d;
}

static int fails = 0, total = 0;
static void check(bool ok, const std::string &name, const char *detail = nullptr) {
  total++; if (!ok) fails++;
  printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(), detail ? "  -> " : "", detail ? detail : "");
}

int main() {
  char err[96];
  auto img = makeImage(130480), dat = makeDat(img);

  { SimBootloader b; const char *e = dfuTransfer(b, img.data(), img.size(), dat.data(), dat.size(), 20, 8, err, sizeof err);
    check(!e && b.activated && b.img == img && b.lastProgress == img.size(), "130,480-byte image, 20-byte packets, receipt every 8", e); }
  { SimBootloader b; const char *e = dfuTransfer(b, img.data(), img.size(), dat.data(), dat.size(), 244, 8, err, sizeof err);
    check(!e && b.activated && b.img == img, "same image, 244-byte packets (current bootloader)", e); }
  { SimBootloader b; b.maxPkt = 20; const char *e = dfuTransfer(b, img.data(), img.size(), dat.data(), dat.size(), 244, 8, err, sizeof err);
    check(e && !b.activated, "244-byte packets to an old bootloader (20-byte limit) fail cleanly, not activated", e); }
  { SimBootloader b; b.maxPkt = 20; const char *e = dfuTransfer(b, img.data(), img.size(), dat.data(), dat.size(), 20, 8, err, sizeof err);
    check(!e && b.activated && b.img == img, "retry with 20-byte packets on the old bootloader succeeds", e); }
  { SimBootloader b; b.dropReceiptNo = 100; const char *e = dfuTransfer(b, img.data(), img.size(), dat.data(), dat.size(), 20, 8, err, sizeof err);
    check(e && !b.activated, "a lost receipt stops the update, nothing activated", e); }
  { auto bad = dat; bad.back() ^= 0x5A; SimBootloader b; const char *e = dfuTransfer(b, img.data(), img.size(), bad.data(), bad.size(), 20, 8, err, sizeof err);
    check(e && !b.activated && strstr(e, "checksum"), "a wrong checksum is caught at the check step, nothing activated", e); }
  { SimBootloader b; b.maxApp = 100000; const char *e = dfuTransfer(b, img.data(), img.size(), dat.data(), dat.size(), 20, 8, err, sizeof err);
    check(e && !b.activated && strstr(e, "too big"), "an image too big for the node is refused at the start", e); }
  { SimBootloader b; const char *e = dfuTransfer(b, img.data(), img.size() - 2, dat.data(), dat.size(), 20, 8, err, sizeof err);
    check(e && b.st == SimBootloader::IDLE && b.q.empty() && !b.activated, "an image that isn't a multiple of 4 is refused before anything is sent", e); }
  { auto img2 = makeImage(20 * 8 * 50), dat2 = makeDat(img2); SimBootloader b;   // last packet completes a receipt group
    const char *e = dfuTransfer(b, img2.data(), img2.size(), dat2.data(), dat2.size(), 20, 8, err, sizeof err);
    check(!e && b.activated && b.img == img2, "image ending exactly on a receipt (late receipt skipped)", e); }
  { auto img2 = makeImage(20 * 8 * 50), dat2 = makeDat(img2); SimBootloader b; b.lateReceiptAtEnd = false;
    const char *e = dfuTransfer(b, img2.data(), img2.size(), dat2.data(), dat2.size(), 20, 8, err, sizeof err);
    check(!e && b.activated, "same, with a bootloader that sends no receipt at the end", e); }
  { SimBootloader b; b.st = SimBootloader::RECV; const char *e = dfuTransfer(b, img.data(), img.size(), dat.data(), dat.size(), 20, 8, err, sizeof err);
    check(e && !b.activated && strstr(e, "wrong state"), "a bootloader stuck mid-update answers 'wrong state'", e); }

  struct { const char *a, *b; int want; } v[] = {{"0.10.0", "0.9.1", 1}, {"0.9.1", "0.10.0", -1}, {"0.10.0", "0.10.0", 0},
    {"1.0", "0.99.99", 1}, {"0.10.0", "0.10", 0}, {"0.9.1-crashtest", "0.9.1", 0}, {"0.10.1", "0.10.0", 1}, {"", "0.9.1", -1}};
  for (auto &t : v) {
    int got = dfuVersionCompare(t.a, t.b);
    char name[64]; snprintf(name, sizeof name, "version compare %s vs %s = %d", t.a[0] ? t.a : "(none)", t.b, t.want);
    check(got == t.want, name);
  }
  printf("%d of %d passed\n", total - fails, total);
  return fails ? 1 : 0;
}

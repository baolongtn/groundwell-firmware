// Host test for the hub's web update check (hub/GroundwellHub/ota_check.h).
//
// Feeds files through the same path as handleUpdateUpload(): the web server's 1436-byte pieces, and a fake
// Update library whose progress() only moves once it has a full 4 KB sector, like the real one. Also runs
// the v0.11.0 check on the same input to show the bug it fixes: that one refused every real hub image.
//
//   g++ -O2 -Wall -Wextra -o /tmp/ota_check_test tools/ota_check_test.cpp && /tmp/ota_check_test
//   /tmp/ota_check_test build/hub/GroundwellHub.ino.bin=accept build/hub/GroundwellHub.ino.merged.bin=refuse ...
//
// With no arguments it runs synthetic images; with file=accept|refuse arguments it checks real build files.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../hub/GroundwellHub/ota_check.h"

static const size_t WEB_PIECE = 1436;      // HTTP_UPLOAD_BUFLEN in the ESP32 WebServer
static const size_t SECTOR = 4096;         // SPI_FLASH_SEC_SIZE: Update writes, and advances progress, per sector
static const uint16_t C6 = 0x000D, S3 = 0x0009;

struct FakeUpdate {                        // just the part of UpdateClass that matters here
  size_t buffered = 0, progressed = 0;
  bool running = true;
  size_t progress() const { return progressed; }
  void write(size_t n) { buffered += n; while (buffered >= SECTOR) { buffered -= SECTOR; progressed += SECTOR; } }
  void abort() { running = false; }
};

// handleUpdateUpload() as of v0.11.1. Returns true if the upload is accepted.
static bool runNew(const std::vector<uint8_t> &f, const std::vector<size_t> &pieces, uint16_t chip) {
  GwImgCheck c; gwImgReset(c);
  FakeUpdate u;
  size_t off = 0;
  for (size_t i = 0; off < f.size(); i++) {
    size_t n = pieces[i < pieces.size() ? i : pieces.size() - 1];
    if (n > f.size() - off) n = f.size() - off;
    if (gwImgFeed(c, f.data() + off, n, chip) == GW_IMG_BAD) { u.abort(); return false; }
    u.write(n); off += n;
  }
  return gwImgAccepted(c);
}

// handleUpdateUpload() as of v0.9.1 to v0.11.0, kept to show the bug.
static bool runOld(const std::vector<uint8_t> &f, const std::vector<size_t> &pieces, uint16_t chip) {
  FakeUpdate u;
  size_t off = 0;
  for (size_t i = 0; off < f.size(); i++) {
    size_t n = pieces[i < pieces.size() ? i : pieces.size() - 1];
    if (n > f.size() - off) n = f.size() - off;
    if (u.progress() == 0) {
      const uint8_t *b = f.data() + off;
      bool image = n >= 36 && b[0] == 0xE9;
      bool app = image && b[32] == 0x32 && b[33] == 0x54 && b[34] == 0xCD && b[35] == 0xAB;
      bool chipOk = image && (b[12] | (b[13] << 8)) == chip;
      if (!app || !chipOk) { u.abort(); return false; }
    }
    u.write(n); off += n;
  }
  return true;
}

static std::vector<uint8_t> appImage(uint16_t chip, size_t len) {
  std::vector<uint8_t> f(len);
  for (size_t i = 0; i < len; i++) f[i] = (uint8_t)(i * 37 + 11);   // filler that is never a header
  f[0] = 0xE9; f[12] = chip & 0xFF; f[13] = chip >> 8;
  f[32] = 0x32; f[33] = 0x54; f[34] = 0xCD; f[35] = 0xAB;
  return f;
}

static int fails = 0, cases = 0;
static void expect(const char *name, bool got, bool want) {
  cases++;
  printf("%s %s: %s\n", got == want ? "PASS" : "FAIL", name, got ? "accepted" : "refused");
  if (got != want) fails++;
}

static bool readFile(const char *path, std::vector<uint8_t> &f) {
  FILE *fp = fopen(path, "rb");
  if (!fp) return false;
  uint8_t buf[65536]; size_t n;
  while ((n = fread(buf, 1, sizeof buf, fp)) > 0) f.insert(f.end(), buf, buf + n);
  fclose(fp);
  return true;
}

int main(int argc, char **argv) {
  const std::vector<size_t> web = {WEB_PIECE}, bytes = {1}, tiny = {20, WEB_PIECE}, sector = {SECTOR}, big = {65536};

  if (argc > 1) {                                    // real build files
    for (int i = 1; i < argc; i++) {
      std::string a = argv[i];
      size_t eq = a.rfind('=');
      if (eq == std::string::npos) { printf("usage: file=accept|refuse\n"); return 2; }
      std::string path = a.substr(0, eq), want = a.substr(eq + 1);
      std::vector<uint8_t> f;
      if (!readFile(path.c_str(), f)) { printf("FAIL %s: can't read it\n", path.c_str()); fails++; continue; }
      bool w = want == "accept";
      std::string n1 = path + " (1436-byte pieces)", n2 = path + " (1-byte pieces)";
      expect(n1.c_str(), runNew(f, web, C6), w);
      expect(n2.c_str(), runNew(f, bytes, C6), w);
    }
    printf("%d of %d passed\n", cases - fails, cases);
    return fails ? 1 : 0;
  }

  std::vector<uint8_t> hub = appImage(C6, 1861056);
  expect("hub image, 1436-byte pieces (what a browser sends)", runNew(hub, web, C6), true);
  expect("hub image, 1-byte pieces", runNew(hub, bytes, C6), true);
  expect("hub image, 20-byte first piece", runNew(hub, tiny, C6), true);
  expect("hub image, 4096-byte pieces", runNew(hub, sector, C6), true);
  expect("hub image, 64 KB pieces", runNew(hub, big, C6), true);
  expect("hub image, chip check off", runNew(hub, web, GW_IMG_ANY_CHIP), true);

  expect("ESP32-S3 image (Nano ESP32 node)", runNew(appImage(S3, 900000), web, C6), false);
  std::vector<uint8_t> merged = appImage(C6, 2000000);
  merged[32] = 0; merged[33] = 0;                    // a bootloader image at 0x0: no app descriptor at 32
  expect("merged file (bootloader first)", runNew(merged, web, C6), false);
  std::vector<uint8_t> hex(300000, ':');
  expect("node .hex (text)", runNew(hex, web, C6), false);
  std::vector<uint8_t> shortHub(hub.begin(), hub.begin() + 20);
  expect("20-byte file (shorter than a header)", runNew(shortHub, web, C6), false);
  std::vector<uint8_t> empty;
  expect("empty file", runNew(empty, web, C6), false);

  // The bug this replaces: v0.11.0's check refuses a good image as the browser sends it, because
  // Update.progress() is still 0 on the second 1436-byte piece.
  expect("v0.11.0 check, hub image in 1436-byte pieces (the bug)", runOld(hub, web, C6), false);
  expect("v0.11.0 check, hub image in 4096-byte pieces (why it looked fine)", runOld(hub, sector, C6), true);

  printf("%d of %d passed\n", cases - fails, cases);
  return fails ? 1 : 0;
}

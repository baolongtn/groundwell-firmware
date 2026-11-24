// Checks gw_hmac.h (used by both the node and the hub) against the RFC 4231 HMAC-SHA256 test vectors
// and FIPS 180-4 SHA-256 examples.  Build and run:  g++ -O2 -o hmac_test hmac_test.cpp && ./hmac_test
#include <cstdio>
#include <string>
#include <vector>
#include "../node/GroundwellNode/gw_hmac.h"

static std::vector<uint8_t> hex(const char *s) {
  std::vector<uint8_t> v;
  for (; s[0] && s[1]; s += 2) { unsigned b; sscanf(s, "%2x", &b); v.push_back((uint8_t)b); }
  return v;
}
static std::string tohex(const uint8_t *p, size_t n) {
  std::string s; char b[3];
  for (size_t i = 0; i < n; i++) { snprintf(b, sizeof b, "%02x", p[i]); s += b; }
  return s;
}

int main() {
  int fails = 0, n = 0;
  struct { const char *msg, *want; } sha[] = {
    {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
    {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
  };
  for (auto &t : sha) {
    GwSha256 s; uint8_t out[32];
    gw_sha256_init(&s); gw_sha256_update(&s, (const uint8_t *)t.msg, strlen(t.msg)); gw_sha256_final(&s, out);
    bool ok = tohex(out, 32) == t.want; n++; fails += !ok;
    printf("%s SHA-256(\"%.12s...\")\n", ok ? "PASS" : "FAIL", t.msg);
  }
  {   // one million 'a' (FIPS 180-2 example), fed in odd-sized pieces
    GwSha256 s; uint8_t out[32]; std::string chunk(997, 'a'); size_t left = 1000000;
    gw_sha256_init(&s);
    while (left) { size_t k = left < chunk.size() ? left : chunk.size(); gw_sha256_update(&s, (const uint8_t *)chunk.data(), k); left -= k; }
    gw_sha256_final(&s, out);
    bool ok = tohex(out, 32) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"; n++; fails += !ok;
    printf("%s SHA-256(1,000,000 x 'a')\n", ok ? "PASS" : "FAIL");
  }
  // RFC 4231 test cases 1-4, 6, 7 (case 5 is a truncated output; 6 and 7 use a 131-byte key)
  std::string k6(131, '\xaa');
  struct { std::vector<uint8_t> key, data; const char *want; const char *name; } hm[] = {
    {std::vector<uint8_t>(20, 0x0b), hex("4869205468657265"), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", "RFC 4231 case 1"},
    {hex("4a656665"), hex("7768617420646f2079612077616e7420666f72206e6f7468696e673f"), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", "RFC 4231 case 2"},
    {std::vector<uint8_t>(20, 0xaa), std::vector<uint8_t>(50, 0xdd), "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe", "RFC 4231 case 3"},
    {hex("0102030405060708090a0b0c0d0e0f10111213141516171819"), std::vector<uint8_t>(50, 0xcd), "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b", "RFC 4231 case 4"},
    {std::vector<uint8_t>(k6.begin(), k6.end()), hex("54657374205573696e67204c6172676572205468616e20426c6f636b2d53697a65204b6579202d2048617368204b6579204669727374"), "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", "RFC 4231 case 6"},
    {std::vector<uint8_t>(k6.begin(), k6.end()), hex("5468697320697320612074657374207573696e672061206c6172676572207468616e20626c6f636b2d73697a65206b657920616e642061206c6172676572207468616e20626c6f636b2d73697a6520646174612e20546865206b6579206e6565647320746f20626520686173686564206265666f7265206265696e6720757365642062792074686520484d414320616c676f726974686d2e"), "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2", "RFC 4231 case 7"},
  };
  for (auto &t : hm) {
    uint8_t out[32];
    gw_hmac_sha256(t.key.data(), t.key.size(), t.data.data(), t.data.size(), out);
    bool ok = tohex(out, 32) == t.want; n++; fails += !ok;
    printf("%s HMAC-SHA256 %s\n", ok ? "PASS" : "FAIL", t.name);
  }
  uint8_t a[16] = {1, 2, 3}, b[16] = {1, 2, 3}, c[16] = {1, 2, 4};
  bool ok = gw_equal(a, b, 16) && !gw_equal(a, c, 16); n++; fails += !ok;
  printf("%s constant-time compare\n", ok ? "PASS" : "FAIL");
  printf("%d of %d passed\n", n - fails, n);
  return fails ? 1 : 0;
}

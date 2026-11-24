// GROUNDWELL hub credentials (optional since v0.9.1).
// Copy this file to secrets.h (same folder) and fill it in. secrets.h is git-ignored,
// so your Wi-Fi password and ingest key never reach GitHub.
// Without secrets.h the hub opens its setup hotspot on first boot instead. With it, the values fill in
// an empty hub, and replace the saved ones only when this file changes. Day to day, use /setup.
#pragma once

#define WIFI_SSID   "your-2.4GHz-network"
#define WIFI_PASS   "your-wifi-password"
#define CLOUD_URL   "https://groundwell.<your-subdomain>.workers.dev/api/ingest"   // "" = no upload
#define INGEST_KEY  "same-string-as-the-INGEST_KEY-secret-in-Cloudflare"
// #define OTA_PASS "choose-8-or-more-characters"   // optional: the update password (or set it on /setup)

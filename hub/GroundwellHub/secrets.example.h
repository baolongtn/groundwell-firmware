// GROUNDWELL hub credentials.
// Copy this file to secrets.h (same folder) and fill it in. secrets.h is git-ignored,
// so your Wi-Fi password and ingest key never reach GitHub.
#pragma once

#define WIFI_SSID   "your-2.4GHz-network"
#define WIFI_PASS   "your-wifi-password"
#define CLOUD_URL   "https://groundwell.<your-subdomain>.workers.dev/api/ingest"   // "" = no upload
#define INGEST_KEY  "same-string-as-the-INGEST_KEY-secret-in-Cloudflare"

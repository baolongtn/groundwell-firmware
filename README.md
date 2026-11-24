# groundwell-firmware

Firmware for GROUNDWELL: the XIAO nRF52840 sensor node and the XIAO ESP32-C6 hub. Current version: **node v0.9.0, hub v0.9.0**.

The cloud (Cloudflare Worker + D1, dashboard, health engine) lives in [groundwell-cloud](https://github.com/baolongtn/groundwell-cloud).

| Folder | What it is |
|---|---|
| `node/GroundwellNode` | Arduino sketch for the XIAO nRF52840: soil probes, air, light and UV, broadcast as BTHome v2 |
| `hub/GroundwellHub` | Arduino sketch for the XIAO ESP32-C6: hears the nodes, serves the live dashboard, uploads to the cloud |
| `tools` | `factory_cal.py` (factory calibration fixture), `fake_node_pty.py` (fake node for testing the fixture), and the packet, photoresistor and sensor-math tests |
| `legacy/wired-prototype` | The original wired ESP32-C6 sketches and the light-sensor test sketch |
| `docs/SETUP.md` | Full setup guide: wiring, flashing, factory calibration, cloud, dashboards, verification |

## Quick start

**Node:** Arduino IDE, board package "Seeed nRF52 Boards" (not mbed), board "Seeed XIAO nRF52840", library "DHT sensor library" by Adafruit. Open `node/GroundwellNode` and upload. Then run factory calibration: `cd tools && python factory_cal.py --operator BN`.

**Hub:** copy `hub/GroundwellHub/secrets.example.h` to `secrets.h` in the same folder and fill in your Wi-Fi, `CLOUD_URL` and `INGEST_KEY`. Board "XIAO_ESP32C6", partition scheme **Huge APP (3MB No OTA)**, then upload.

Details for both are in [`docs/SETUP.md`](docs/SETUP.md), sections 1, 2, 2b and 4.

## Secrets

`secrets.h` is git-ignored. The hub won't compile without it, and the error message says what to do. Keep `INGEST_KEY.txt` outside the repo.

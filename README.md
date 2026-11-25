# groundwell-firmware

Firmware for GROUNDWELL: the XIAO nRF52840 sensor node and the XIAO ESP32-C6 hub. Current version: **node v0.12.0, hub v0.12.0** (the Arduino Nano ESP32 test node builds from the same sketch at the same version). A hub on v0.11.0 or older can't take browser updates: move it to v0.11.1 or later once from the Arduino IDE or `espota.py` ([SETUP.md section 6](docs/SETUP.md#hub)). Update the hub over the air and it updates its nodes over Bluetooth ([SETUP.md section 6](docs/SETUP.md#6-update-over-the-air)). The hub can carry its own sensors too, wired like a node ([SETUP.md section 4b](docs/SETUP.md#4b-optional-sensors-on-the-hub)). A health check of the whole chain, nodes to cloud, with state, traffic and an event log: `http://plants.local/diag`.

The cloud (Cloudflare Worker + D1, dashboard, health engine) lives in [groundwell-cloud](https://github.com/baolongtn/groundwell-cloud).

| Folder | What it is |
|---|---|
| `node/GroundwellNode` | Arduino sketch for the XIAO nRF52840: capacitive soil probe, air, light and UV, broadcast as BTHome v2 |
| `hub/GroundwellHub` | Arduino sketch for the XIAO ESP32-C6: hears the nodes, reads its own sensors if wired (`local_sensors.h`, the node's code), serves the live dashboard and health checks, uploads to the cloud |
| `tools` | `factory_cal.py` (factory calibration fixture, nodes and the hub's own probe), `fake_node_pty.py` (fake node and hub for testing the fixture), `make_node_fw_h.py` (bundles the node firmware into the hub, and gzips the hub's pages with `make_pages_gz.py`), `check_shared_code.py` (the hub's copy of the node's sensor code must match), and host tests: hub packet handling, hub web update check, node update protocol against a simulated bootloader, link-key HMAC, packets, photoresistor, sensor math |
| `legacy/wired-prototype` | The original wired ESP32-C6 sketches and the light-sensor test sketch |
| `docs/SETUP.md` | Full setup guide: wiring, flashing, factory calibration, cloud, dashboards, verification |

## Quick start

**Node:** Arduino IDE, board package "Seeed nRF52 Boards" (not mbed), board "Seeed XIAO nRF52840", library "DHT sensor library" by Adafruit. Open `node/GroundwellNode` and upload. Then run factory calibration: `cd tools && python factory_cal.py --operator BN`.

**Test node on an Arduino Nano ESP32:** the same sketch builds for it (board package "esp32" 3.x, board "Arduino Nano ESP32"). It sends the same packet but is broadcast only, so the hub never links or updates it. It has its own pin map; see `docs/SETUP.md` section 2c.

**Hub:** board package "esp32" **3.x** (not 4.0), board "XIAO_ESP32C6", partition scheme **Huge APP** (only for the IDE's size check; the sketch brings its own two-slot table), library "DHT sensor library" by Adafruit. Optionally copy `hub/GroundwellHub/secrets.example.h` to `secrets.h` and fill it in; without it the hub opens a setup hotspot. Upload, then set an update password at `http://plants.local/setup`, and tick any sensors wired to the hub there.

**Without Arduino IDE:** every push builds both boards on GitHub (Actions > latest run > Artifacts): `.uf2` for the node, `.merged.bin` for the hub's web flasher, and the files for over-the-air updates.

Details in [`docs/SETUP.md`](docs/SETUP.md), sections 1, 2, 2b, 4 and 6.

## Secrets

`secrets.h` is git-ignored and optional. It only fills in an empty hub (or one whose `secrets.h` changed); day to day, settings live in the hub and change on `/setup`. Keep `INGEST_KEY.txt` outside the repo.

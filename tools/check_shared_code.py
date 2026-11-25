#!/usr/bin/env python3
"""Fails if the hub's copy of the node's measurement code (hub/GroundwellHub/local_sensors.h) has drifted from
the node sketch. The hub reads its own sensors with these exact functions, so a probe wired to the hub and one
on a node give the same numbers, the same packet and the same factory calibration.

  python3 tools/check_shared_code.py          (from the repo root; the GitHub build runs it)
"""
import os, sys

# (name, start anchor, end anchor): each block runs from its start anchor up to its end anchor in the node sketch,
# and must appear word for word in the hub's header.
BLOCKS = [
    ("sensor math",      "// ---- sensor math (host-tested",              "// ---- end sensor math ----"),
    ("capacitive probe", "static uint16_t avgRead(int pin, uint8_t n) {", "// Supply voltage through the SAADC"),
    ("I2C sensors",      "static bool i2cWrite(uint8_t addr",              "#else\nstatic bool readUV(float &) { return false; }"),
    ("DHT11",            "static bool readDHT(float &tempC, float &rh) {", "// Best temperature/humidity source"),
    ("photoresistor",    "__attribute__((unused)) static float ldrLux(uint16_t adc) {", "#if LIGHT_SENSOR == LIGHT_I2C\n// VEML7700"),
    ("calibration",      "static uint16_t calCheck(const CalData &c)",     "static const char *lightName()"),
    ("BTHome packet",    "static uint8_t buildBTHome(",                    "// ---- end BTHome packet"),
]

def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    node = open(os.path.join(root, "node", "GroundwellNode", "GroundwellNode.ino"), encoding="utf-8").read()
    hub = open(os.path.join(root, "hub", "GroundwellHub", "local_sensors.h"), encoding="utf-8").read()
    bad = 0
    for name, start, end in BLOCKS:
        try:
            a = node.index(start); b = node.index(end, a)
        except ValueError:
            print(f"FAIL {name}: anchor not found in the node sketch (update tools/check_shared_code.py)"); bad += 1; continue
        block = node[a:b]
        if block in hub:
            print(f"ok   {name} ({len(block.splitlines())} lines)")
        else:
            print(f"FAIL {name}: the hub's copy differs from the node sketch. Copy it from the node into local_sensors.h."); bad += 1
    sys.exit(1 if bad else 0)

if __name__ == "__main__":
    main()

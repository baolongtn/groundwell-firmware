#!/usr/bin/env python3
"""Cuts handlePacket() and the code it needs out of the hub sketch into hub_packet.inc for hub_packet_test.cpp."""
import os
here = os.path.dirname(os.path.abspath(__file__))
s = open(os.path.join(here, "..", "hub", "GroundwellHub", "GroundwellHub.ino"), encoding="utf-8").read()
def cut(a, b):
    i = s.index(a); return s[i:s.index(b, i)]
parts = [
    cut("static const int MAX_NODES", "Node nodes[MAX_NODES];") + "Node nodes[MAX_NODES];\n",
    cut("Sensor sensors[MAX_SENSORS];", "uint32_t lastDayPush"),
    cut("// ---------------- helpers ----------------", "class ScanCallbacks"),
    cut("// Health window (v0.12)", "// ---------------- cloud upload"),
]
open(os.path.join(here, "hub_packet.inc"), "w").write("\n".join(parts))
print("hub_packet.inc:", sum(p.count("\n") for p in parts), "lines")

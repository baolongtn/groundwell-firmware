#!/usr/bin/env python3
"""
GROUNDWELL factory calibration fixture (node firmware 0.6+).

Runs one node (or a batch) through: identify -> timing check -> air point -> water point
-> verify in water -> verify back in air, then appends one row per unit to a CSV log.

Setup (once):   pip install pyserial
Run:            python factory_cal.py                      (finds the XIAO automatically)
                python factory_cal.py --port COM7 --operator BN --batch
Close Arduino's Serial Monitor first: only one program can hold the port.

Reference conditions (keep them the same for every unit):
  AIR    probe clean and dry, held in open air at least 5 cm from hands, metal and the bench
  WATER  distilled or deionized water at room temperature (about 22 C), probe immersed to its
         max line, at least 3 cm from the container walls and bottom
"""
import argparse
import csv
import datetime as dt
import os
import re
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("pyserial is missing: run  pip install pyserial")

INTERVAL_MS = 5000
CADENCE_TOL_MS = 500          # each reading must arrive 4.5 to 5.5 s after the last
WATER_MIN_PCT = 95.0          # verify step: probe in water must read at least this
AIR_MAX_PCT = 5.0             # verify step: probe back in air must read at most this
SEEED_VID = 0x2886
LOG_FIELDS = ["time_utc", "operator", "mac", "fw", "result", "reason", "air", "water", "span",
              "air_sd", "water_sd", "cadence_ms_min", "cadence_ms_max", "verify_water_pct", "verify_air_pct"]

DATA_RE = re.compile(r"^#(\d+) dt=(\d+)ms\s+capacitive\s+(\d+)\s*(?:=\s*([\d.]+)%|\(uncal\))")


class Node:
    def __init__(self, port, verbose):
        self.s = serial.Serial(port, 115200, timeout=0.2)
        self.verbose = verbose
        self.buf = b""
        self.seen = []

    def lines(self, seconds):
        """Yield decoded lines for up to `seconds`."""
        end = time.time() + seconds
        while time.time() < end:
            chunk = self.s.read(256)
            if chunk:
                self.buf += chunk
                while b"\n" in self.buf:
                    raw, self.buf = self.buf.split(b"\n", 1)
                    line = raw.decode("utf-8", "replace").strip()
                    if line:
                        if self.verbose:
                            print("   node> " + line)
                        yield line

    def command(self, cmd, want, seconds):
        """Send a command and return the first reply line matching regex `want`, or None."""
        self.s.reset_input_buffer()
        self.buf = b""
        self.s.write((cmd + "\n").encode())
        pat = re.compile(want)
        self.seen = []
        for line in self.lines(seconds):
            self.seen.append(line)
            if pat.search(line):
                return line
        return None

    def readings(self, n, seconds):
        """Collect up to n data lines: list of (pid, dt_ms, raw, pct or None)."""
        out = []
        for line in self.lines(seconds):
            m = DATA_RE.match(line)
            if m:
                out.append((int(m.group(1)), int(m.group(2)), int(m.group(3)),
                            float(m.group(4)) if m.group(4) else None))
                if len(out) >= n:
                    break
        return out

    def close(self):
        self.s.close()


def find_port():
    ports = [p for p in serial.tools.list_ports.comports() if p.vid == SEEED_VID]
    if len(ports) == 1:
        return ports[0].device
    if not ports:
        sys.exit("No XIAO found. Plug the node in by USB, or pass --port (for example --port COM7).")
    sys.exit("More than one XIAO is plugged in: " + ", ".join(p.device for p in ports) + ". Pass --port.")


def kv(line, key):
    m = re.search(r"\b" + key + r"=([^\s]+)", line or "")
    return m.group(1) if m else ""


def ask(prompt, auto):
    if auto is not None:
        print(prompt + " [auto]")
        time.sleep(auto)
        return
    input(prompt + "  Press Enter when ready... ")


def run_unit(port, args):
    row = {k: "" for k in LOG_FIELDS}
    row["time_utc"] = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    row["operator"] = args.operator
    node = Node(port, args.verbose)
    try:
        time.sleep(1.5)                                   # opening the port can reset some boards
        info = node.command("INFO", r"^INFO ", 8)
        if not info:
            return fail(row, "no_reply", "The node didn't answer INFO. Is it running firmware 0.6 or later?")
        row["mac"], row["fw"] = kv(info, "mac"), kv(info, "fw")
        print(f"Node {row['mac']}  firmware {row['fw']}  calibrated: {kv(info, 'cal')}")
        # Firmware 0.9+ reads every 30 s to save power; FAST switches it to 5 s for the next 5 minutes.
        # Older nodes answer "ERR unknown command" and already run at 5 s.
        node.command("FAST", r"^(FAST|ERR)", 3)
        if kv(info, "cal") == "yes" and not args.force:
            if args.auto is None and input("Already calibrated. Recalibrate? [y/N] ").strip().lower() != "y":
                return fail(row, "skipped", "Left as it was.")

        # 1. Timing: the node must send one reading every 5 s.
        print(f"Checking timing for {args.cadence_s:.0f} s...")
        rd = node.readings(99, args.cadence_s)
        dts = [r[1] for r in rd if r[1] > 0]
        if len(dts) < 2:
            return fail(row, "no_readings", f"Only {len(rd)} readings in {args.cadence_s:.0f} s.")
        row["cadence_ms_min"], row["cadence_ms_max"] = min(dts), max(dts)
        if min(dts) < INTERVAL_MS - CADENCE_TOL_MS or max(dts) > INTERVAL_MS + CADENCE_TOL_MS:
            return fail(row, "timing", f"Readings every {min(dts)} to {max(dts)} ms; expected {INTERVAL_MS} +- {CADENCE_TOL_MS}.")
        print(f"  OK: every {min(dts)} to {max(dts)} ms")

        # 2. Air point
        for attempt in (1, 2):
            ask("AIR: probe clean, dry, in open air, 5 cm from hands and metal.", args.auto)
            line = node.command("CAL AIR", r"^CAL AIR ", 30)
            row["air_sd"] = kv(line, "sd")
            if line and line.endswith(" OK"):
                print(f"  OK: {line}")
                break
            print(f"  {line or 'no reply'}  (keep the probe still and try again)")
        else:
            return fail(row, "air_unstable", "Air reading wasn't steady.")

        # 3. Water point -> the node checks limits and saves
        for attempt in (1, 2):
            ask("WATER: probe in distilled water up to its max line, 3 cm from the walls.", args.auto)
            line = node.command("CAL WATER", r"^CAL RESULT |^CAL WATER .*FAIL", 30)
            if line and line.startswith("CAL WATER"):
                print(f"  {line}  (keep the probe still and try again)")
                continue
            break
        if not line:
            return fail(row, "no_reply", "No calibration result.")
        if "PASS" not in line:
            row["air"], row["water"] = kv(line, "air"), kv(line, "water")
            return fail(row, kv(line, "reason") or "fail", line)
        row["air"], row["water"], row["span"] = kv(line, "air"), kv(line, "water"), kv(line, "span")
        row["water_sd"] = next((kv(l, "sd") for l in node.seen if l.startswith("CAL WATER")), "")
        print(f"  {line}")

        # 4. Verify: still in water it must read near 100 %. Each verify skips the first reading,
        #    which may have been taken before or while the probe moved.
        rd = node.readings(3, 18)[1:]
        pcts = [r[3] for r in rd if r[3] is not None]
        if not pcts:
            return fail(row, "verify", "No calibrated readings after saving.")
        row["verify_water_pct"] = f"{sum(pcts) / len(pcts):.1f}"
        if min(pcts) < WATER_MIN_PCT:
            return fail(row, "verify_water", f"Reads {min(pcts):.1f}% in water; expected at least {WATER_MIN_PCT}%.")
        print(f"  In water: {row['verify_water_pct']}%")

        # 5. Verify: back in air it must read near 0 %
        ask("VERIFY: take the probe out, dry it, hold it in open air again.", args.auto)
        node.s.reset_input_buffer()
        node.buf = b""
        rd = node.readings(3, 18)[1:]
        pcts = [r[3] for r in rd if r[3] is not None]
        if not pcts:
            return fail(row, "verify", "No readings in air.")
        row["verify_air_pct"] = f"{sum(pcts) / len(pcts):.1f}"
        if max(pcts) > AIR_MAX_PCT:
            return fail(row, "verify_air", f"Reads {max(pcts):.1f}% in air; expected at most {AIR_MAX_PCT}%. Was it dry?")
        print(f"  In air: {row['verify_air_pct']}%")

        row["result"] = "PASS"
        print(f"PASS  {row['mac']}  air {row['air']}  water {row['water']}  span {row['span']}")
        return row
    finally:
        node.close()
        log(args.log, row)


def fail(row, reason, msg):
    row["result"], row["reason"] = "FAIL", reason
    print(f"FAIL  {row.get('mac') or ''}  {msg}")
    return row


def log(path, row):
    if not row.get("result"):
        row["result"], row["reason"] = "FAIL", row.get("reason") or "aborted"
    new = not os.path.exists(path)
    with open(path, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=LOG_FIELDS)
        if new:
            w.writeheader()
        w.writerow(row)


def main():
    ap = argparse.ArgumentParser(description="GROUNDWELL node factory calibration")
    ap.add_argument("--port", help="serial port (default: the one XIAO plugged in)")
    ap.add_argument("--operator", default=os.environ.get("USERNAME") or os.environ.get("USER") or "")
    ap.add_argument("--log", default="factory_cal_log.csv", help="CSV log, one row per unit (appended)")
    ap.add_argument("--batch", action="store_true", help="keep going: calibrate the next node after each one")
    ap.add_argument("--force", action="store_true", help="recalibrate without asking if the node is already calibrated")
    ap.add_argument("--cadence-s", type=float, default=16, help="how long to watch the timing (s)")
    ap.add_argument("--auto", type=float, default=None, help=argparse.SUPPRESS)   # tests: wait N s instead of prompting
    ap.add_argument("-v", "--verbose", action="store_true", help="echo everything the node prints")
    args = ap.parse_args()
    while True:
        run_unit(args.port or find_port(), args)
        if not args.batch or input("\nNext node? Plug it in and press Enter (or type q): ").strip().lower() == "q":
            break
    print(f"Log: {os.path.abspath(args.log)}")


if __name__ == "__main__":
    main()

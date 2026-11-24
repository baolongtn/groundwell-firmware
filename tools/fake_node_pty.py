# Fake GROUNDWELL node (firmware 0.6 / 0.9 serial protocol) on a pseudo-terminal, for testing factory_cal.py on Linux or macOS.
# Usage: python3 fake_node_pty.py good|fast|bad_probe|v09   (prints the port; then run factory_cal.py --port <it> --auto 2 --cadence-s 3)
import os, pty, sys, time, select, random
scen = sys.argv[1]
master, slave = pty.openpty()
import tty; tty.setraw(slave)
print(os.ttyname(slave), flush=True)
AIR, WATER = 3115, (2800 if scen == 'bad_probe' else 1450)
medium, cal, pend, pid, after = 'air', None, {}, 0, 0
dt_claim = 400 if scen == 'fast' else 30000 if scen == 'v09' else 5000   # v09: 30 s until FAST
def w(s): os.write(master, (s + "\r\n").encode())
def raw(): return (AIR if medium == 'air' else WATER) + random.randint(-2, 2)
buf = b""; last = time.time()
while True:
    r, _, _ = select.select([master], [], [], 0.05)
    if r:
        buf += os.read(master, 256)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1); cmd = line.decode().strip().upper()
            if cmd == 'INFO': w(f"INFO fw={'0.9.0' if scen == 'v09' else '0.6.0'} mac=AA:BB:CC:DD:1F:C1 interval_ms={dt_claim} light=ldr " + (f"cal=yes air={cal[0]} water={cal[1]} OK" if cal else "cal=no OK"))
            elif cmd == 'FAST':
                if scen == 'v09': dt_claim = 5000; w("FAST interval_ms=5000 for_s=300 OK")
                else: w("ERR unknown command: FAST")
            elif cmd in ('CAL AIR', 'CAL WATER'):
                medium = 'air' if cmd == 'CAL AIR' else 'water'; time.sleep(1)
                v = raw(); pend[medium] = v; w(f"CAL {medium.upper()} mean={v}.0 sd=1.1 n=20 OK")
                if len(pend) == 2:
                    a, wt = pend['air'], pend['water']
                    if wt > 2600: w(f"CAL RESULT FAIL reason=water_out_of_range air={a} water={wt}")
                    else: cal = (a, wt); after = 0; w(f"CAL RESULT PASS air={a} water={wt} span={a-wt} saved")
    if time.time() - last > 0.5:
        last = time.time(); pid += 1
        if cal and after >= 3: medium = 'air'
        v = raw()
        if cal: after += 1; p = max(0, min(100, (cal[0] - v) * 100 / (cal[0] - cal[1]))); w(f"#{pid} dt={dt_claim}ms  capacitive {v} = {p:5.1f}%  rods    2  air 22.0 C 45%RH  light 900 lux (adc 1200)  VDD 3.30 V")
        else: w(f"#{pid} dt={dt_claim}ms  capacitive {v} (uncal)  rods    2  air 22.0 C 45%RH  light 900 lux (adc 1200)  VDD 3.30 V")

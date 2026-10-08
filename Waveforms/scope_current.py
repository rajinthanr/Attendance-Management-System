#!/usr/bin/env python3
"""Log the logger's battery current with a Tektronix TBS1000C (TBS1052C) over USB.

The scope reads the voltage across a shunt resistor in the battery's negative
lead; this script fetches each triggered acquisition (up to 20 000 points),
converts it to current, and logs the mean current, the charge and the peak.

    pip install pyvisa pyvisa-py pyusb
    python3 scope_current.py --list
    python3 scope_current.py --zero                    # no current flowing: measures the offset
    python3 scope_current.py --shunt 1.0 --captures 20 --offset-mv 0.12 --out taps

Every capture is saved as <out>_NNN.csv (t_s, v, i_a), and one summary row per
capture goes to <out>_summary.csv. The scope's own settings (V/div, time/div,
trigger, acquisition mode) are used as they are, so set them up on the front
panel first; the script only arms single acquisitions and reads them.

Linux: the scope needs USB access, e.g. /etc/udev/rules.d/99-tek.rules with
    SUBSYSTEM=="usb", ATTR{idVendor}=="0699", MODE="0666"
"""
import argparse
import csv
import sys
import time
from datetime import datetime

try:
    import pyvisa
except ImportError:
    sys.exit("needs pyvisa: pip install pyvisa pyvisa-py pyusb")


def open_scope(rm, resource):
    if resource is None:
        tek = [r for r in rm.list_resources() if "0x0699" in r.lower() or "::1689::" in r]
        if not tek:
            sys.exit("no Tektronix scope found; check the rear USB cable and --list")
        resource = tek[0]
    scope = rm.open_resource(resource)
    scope.timeout = 20000
    scope.read_termination = "\n"
    scope.write_termination = "\n"
    print("connected:", scope.query("*IDN?").strip())
    return scope


def preamble(scope):
    """Scale factors for the selected source, whichever command family the firmware answers."""
    for prefix in ("WFMOutpre", "WFMPre"):
        try:
            return {k: float(scope.query(f"{prefix}:{k}?"))
                    for k in ("XINcr", "XZEro", "YMUlt", "YOFf", "YZEro")}
        except pyvisa.errors.VisaIOError:
            scope.write("*CLS")
    sys.exit("the scope answered neither WFMOutpre nor WFMPre")


def acquire(scope, channel, timeout_s):
    scope.write("ACQuire:STOPAfter SEQuence")
    scope.write("ACQuire:STATE RUN")
    t0 = time.time()
    while int(scope.query("ACQuire:STATE?").strip()) != 0:
        if time.time() - t0 > timeout_s:
            return None                       # no trigger
        time.sleep(0.05)
    scope.write(f"DATa:SOUrce {channel}")
    scope.write("DATa:ENCdg ASCii")
    scope.write("DATa:WIDth 1")
    scope.write("DATa:STARt 1")
    scope.write("DATa:STOP 20000")
    p = preamble(scope)
    raw = [float(x) for x in scope.query("CURVe?").strip().split(",")]
    t = [p["XZEro"] + i * p["XINcr"] for i in range(len(raw))]
    v = [(r - p["YOFf"]) * p["YMUlt"] + p["YZEro"] for r in raw]
    return t, v, p["XINcr"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true", help="list VISA resources and exit")
    ap.add_argument("--resource", help="VISA resource string (default: the first Tektronix device)")
    ap.add_argument("--channel", default="CH1")
    ap.add_argument("--shunt", type=float, default=1.0, help="shunt resistance in ohms")
    ap.add_argument("--offset-mv", type=float, default=0.0, help="scope offset to subtract, from --zero")
    ap.add_argument("--captures", type=int, default=10)
    ap.add_argument("--wait", type=float, default=30.0, help="seconds to wait for each trigger")
    ap.add_argument("--zero", action="store_true", help="measure the offset with no current flowing")
    ap.add_argument("--out", default="capture")
    a = ap.parse_args()

    rm = pyvisa.ResourceManager("@py")
    if a.list:
        print("\n".join(rm.list_resources()) or "nothing found")
        return
    scope = open_scope(rm, a.resource)

    if a.zero:
        scope.write("TRIGger:FORce")
        got = acquire(scope, a.channel, 5.0) or sys.exit("no acquisition")
        mean_mv = 1000 * sum(got[1]) / len(got[1])
        print(f"offset {mean_mv:+.3f} mV: pass --offset-mv {mean_mv:.3f}")
        return

    with open(f"{a.out}_summary.csv", "w", newline="") as fs:
        summary = csv.writer(fs)
        summary.writerow(["n", "time", "window_s", "points", "mean_ma", "charge_uc", "peak_ma", "file"])
        for n in range(1, a.captures + 1):
            got = acquire(scope, a.channel, a.wait)
            if got is None:
                print(f"{n}: no trigger within {a.wait:g} s")
                continue
            t, v, dt = got
            i = [(x - a.offset_mv / 1000) / a.shunt for x in v]
            window = dt * len(i)
            charge_uc = sum(i) * dt * 1e6          # rectangle rule; fine at these sample rates
            mean_ma = 1000 * sum(i) / len(i)
            name = f"{a.out}_{n:03d}.csv"
            with open(name, "w", newline="") as fw:
                w = csv.writer(fw)
                w.writerow(["t_s", "v", "i_a"])
                w.writerows(zip(t, v, i))
            summary.writerow([n, datetime.now().isoformat(timespec="seconds"), f"{window:.6g}", len(i),
                              f"{mean_ma:.4f}", f"{charge_uc:.3f}", f"{1000*max(i):.2f}", name])
            fs.flush()
            print(f"{n}: {window*1000:.3g} ms window, mean {mean_ma:.3f} mA, "
                  f"charge {charge_uc:.2f} uC, peak {1000*max(i):.1f} mA")


if __name__ == "__main__":
    main()

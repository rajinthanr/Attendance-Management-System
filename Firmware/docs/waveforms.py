"""Battery-current measurements from the TBS1052C captures in Waveforms/, for power_report.py.

The scope read the voltage across a 1 ohm shunt in the battery's negative lead
(probe and menu at 10X, so the CSV values are volts at the shunt); 1 mV is
1 mA. Each capture's DC offset is taken from a quiet stretch of it and
subtracted. The windows below were picked by eye from the plots
(Waveforms/plot_waveforms.py --shunt 1.0).

    TEK00005  0.4 ms/div  one wake-up measurement
    TEK00006  10 ms/div   a card tap: IRQ, read, buzz, start of the LED
    TEK00007  10 ms/div   a false wake-up: measurement, empty poll, re-arm
"""
import csv
import os
import statistics

DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "Waveforms")
SHUNT_OHM = 1.0
MEASURED_ON = "8 October 2026"


def load(name):
    """(t in s, i in mA) from a TEKnnnnn.CSV."""
    t, i = [], []
    with open(os.path.join(DIR, name), newline="") as f:
        rows = csv.reader(f)
        for r in rows:
            if r and r[0].strip().upper() == "TIME":
                break
        for r in rows:
            if len(r) < 2 or not r[0].strip():
                continue
            t.append(float(r[0]))
            i.append(float(r[1]) * 1000.0 / SHUNT_OHM)
    return t, i


class Capture:
    def __init__(self, name, quiet):
        self.name = name
        self.t, self.i = load(name)
        self.dt = self.t[1] - self.t[0]
        q = [v for tt, v in zip(self.t, self.i) if any(a <= tt < b for a, b in quiet)]
        self.offset = statistics.fmean(q)

    def window(self, a, b):
        return [v - self.offset for tt, v in zip(self.t, self.i) if a <= tt < b]

    def charge_uc(self, a, b):
        return sum(self.window(a, b)) * self.dt * 1000.0

    def mean_ma(self, a, b):
        return statistics.fmean(self.window(a, b))

    def above_ms(self, a, b, ma):
        return sum(1 for v in self.window(a, b) if v > ma) * self.dt * 1000.0

    def trace(self, a, b, buckets=500):
        """(t_ms, mean mA) per bucket, offset removed, for plotting."""
        pts = [(tt, v - self.offset) for tt, v in zip(self.t, self.i) if a <= tt < b]
        n = max(1, len(pts) // buckets)
        out = []
        for k in range(0, len(pts) - n + 1, n):
            seg = pts[k:k + n]
            out.append((seg[n // 2][0] * 1000.0, statistics.fmean(v for _, v in seg)))
        return out


def measure():
    wu = Capture("TEK00005.CSV", [(-3.2e-3, -1.2e-3), (0.6e-3, 3.2e-3)])
    tap = Capture("TEK00006.CSV", [(-31e-3, -5e-3)])
    fw = Capture("TEK00007.CSV", [(20e-3, 100e-3)])
    m = dict(wu=wu, tap=tap, fw=fw)
    # One wake-up measurement
    m["q_wu"] = wu.charge_uc(-0.8e-3, 0.5e-3)
    m["wu_osc_uc"] = wu.charge_uc(-0.65e-3, -0.25e-3)
    m["wu_osc_ma"] = wu.mean_ma(-0.65e-3, -0.25e-3)
    m["wu_plateau_ma"] = wu.mean_ma(-0.25e-3, -0.03e-3)
    m["wu_spike_uc"] = wu.charge_uc(-0.03e-3, 0.15e-3)
    m["wu_spike_peak"] = max(wu.window(-0.03e-3, 0.15e-3))
    m["wu_spike_us"] = wu.above_ms(-0.03e-3, 0.15e-3, 40.0) * 1000.0
    # Card tap
    m["q_irq"] = tap.charge_uc(-3.3e-3, -0.05e-3)          # measurement + IRQ + oscillator start
    m["read_ms"] = tap.above_ms(-0.05e-3, 12.3e-3, 120.0)
    m["q_read"] = tap.charge_uc(-0.05e-3, 12.3e-3)
    m["read_ma"] = m["q_read"] / m["read_ms"]
    m["buzz_ms"] = 62.6 - 12.3
    m["q_buzz"] = tap.charge_uc(12.3e-3, 62.6e-3)
    m["buzz_ma"] = tap.mean_ma(12.3e-3, 62.6e-3)
    m["ready_led_ma"] = tap.mean_ma(70e-3, 128e-3)
    # False wake-up
    m["q_false"] = fw.charge_uc(-2.1e-3, 7e-3)               # after the triggering measurement
    m["empty_ms"] = fw.above_ms(-0.1e-3, 6.2e-3, 100.0)
    m["q_empty"] = fw.charge_uc(-0.1e-3, 6.0e-3)
    m["empty_ma"] = m["q_empty"] / m["empty_ms"]
    m["q_rearm"] = fw.charge_uc(6.0e-3, 7.0e-3)
    m["q_wu_fw"] = [fw.charge_uc(-2.7e-3, -2.1e-3), fw.charge_uc(105.0e-3, 106.0e-3)]
    return m


if __name__ == "__main__":
    m = measure()
    for k, v in m.items():
        if isinstance(v, (int, float)):
            print(f"{k:14s} {v:10.3f}")
        elif isinstance(v, list):
            print(f"{k:14s} {v}")

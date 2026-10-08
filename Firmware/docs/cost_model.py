"""Manufacturing cost per unit at 1, 100, 1 000 and 10 000 units, for power_report.py.

Parts come from the LCSC BOM workbook (PCB/bom), with price breaks read on
lcsc.com on PRICES_ON for the parts that dominate the cost; the rest are scaled
from the workbook's minimum-lot prices. Off-board items, freight, duty and
labour are estimates. Every figure is a parameter below: change one and
regenerate the report.
"""
import os

import openpyxl

BOM = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "PCB", "bom",
                   "Attendance_Management_System_BOM_LCSC.xlsx")
PRICES_ON = "8 October 2026"
LKR_PER_USD = 330.0          # 329.98 on 3 October 2026 (exchange-rates.org)
LKR_RATE_ON = "3 October 2026"
SCALES = [1, 100, 1000, 10000]
MARGIN = 0.40                # example gross margin for the selling price

# LCSC price breaks (USD); the 10 000 column, and the 1 000 column where LCSC
# lists no break, are extrapolated (marked * in the report).
BREAKS = {
    "C2928224": ("STM32L432KBU6 (U1)", {1: 5.0754, 100: 3.53, 1000: 3.2465, 10000: 2.95}, (10000,)),
    "C2908147": ("ST25R3916-AQWT (U5)", {1: 5.4396, 100: 3.7528, 1000: 3.40, 10000: 3.00}, (1000, 10000)),
    "C627668": ("MCP73833 charger (U2)", {1: 1.9017, 100: 1.2399, 1000: 1.1184, 10000: 1.00}, (10000,)),
    "C2887324": ("TPS7A0233 LDO (U3)", {1: 0.5572, 100: 0.36, 1000: 0.3353, 10000: 0.30}, (10000,)),
    "C5143397": ("USB4110 USB-C (J1)", {1: 1.3958, 100: 0.8244, 1000: 0.72, 10000: 0.62}, (1000, 10000)),
    "C2689642": ("PTS636 switch, each (SW1-3)", {1: 0.3565, 100: 0.2571, 1000: 0.1867, 10000: 0.1768}, ()),
    "C141588": ("NTC 10k (TH1)", {1: 0.6261, 100: 0.3852, 1000: 0.3114, 10000: 0.28}, (10000,)),
}
OTHER_FACTOR = {1: 1.0, 100: 0.85, 1000: 0.65, 10000: 0.50}   # vs the workbook's minimum-lot price

# JLCPCB assembly (help article "PCB assembly price", read 8 October 2026)
SMT_JOINTS, THT_JOINTS = 265, 18
EXT_TYPES, PART_TYPES = 20, 34

PCB_EACH = {100: 0.30, 1000: 0.24, 10000: 0.18}      # 2-layer 60 x 89 mm; 1 unit: $2 for 5 boards
BATTERY = {1: 6.00, 100: 3.00, 1000: 2.20, 10000: 1.80}   # 1000 mAh LiPo + protection, JST XA lead
MOTOR = {1: 1.00, 100: 0.45, 1000: 0.30, 10000: 0.25}     # 10 mm coin ERM with leads
ENCL = {1: 8.00, 100: 5.00, 1000: 3.90, 10000: 1.20}      # printed; 1k: $3k Al tool + $0.90; 10k: $6k steel + $0.60
PACK = {1: 1.00, 100: 0.50, 1000: 0.30, 10000: 0.20}      # box, label, screws
FREIGHT = {1: 30.0, 100: 60.0, 1000: 350.0, 10000: 1500.0}   # whole order, China to Colombo
LABOUR_MIN = {1: 120, 100: 15, 1000: 8, 10000: 5}            # final assembly, flashing, test
LABOUR_USD_H = 2.5
FIXTURE = {1: 0.0, 100: 0.0, 1000: 300.0, 10000: 600.0}      # pogo-pin flash/test jig
YIELD = {1: 1.0, 100: 0.95, 1000: 0.97, 10000: 0.98}
IMPORT_TAX = 0.25            # duty + 18 % VAT + 2.5 % SSCL + levies, on the CIF value


def _bom_rows():
    ws = openpyxl.load_workbook(BOM, data_only=True)["BOM"]
    rows = []
    for r in ws.iter_rows(min_row=8, values_only=True):
        if r[0] is None or r[24] != "Y":
            continue
        mn, price = int(r[12] or 1), r[13]
        if r[14] == "Y":                      # the alternate is chosen (R6 2k)
            mn, price = int(r[22] or 1), r[23]
        rows.append(dict(qty=int(r[2]), mn=mn, price=float(price) if price not in (None, "") else None,
                         lcsc=r[8]))
    return rows


def _components(rows, boards, scale):
    tot = 0.0
    for r in rows:
        need = r["qty"] * boards
        if r["lcsc"] in BREAKS:
            b = BREAKS[r["lcsc"]][1]
            tot += max(need, 5 if r["lcsc"] == "C2689642" else 1) * b[scale]
        else:
            buy = max(need, r["mn"]) if scale == 1 else need
            tot += buy * r["price"] * OTHER_FACTOR[scale]
    return tot


def _assembly(n):
    if n <= 100:                              # Economic
        return 8.18 + 1.53 + EXT_TYPES * 3.07 + (SMT_JOINTS * 0.0016 + THT_JOINTS * 0.0164) * n + 3.58
    j, t = SMT_JOINTS * n, THT_JOINTS * n     # Standard, tiered per joint
    smt = (min(j, 5e4) * 0.0016 + min(max(j - 5e4, 0), 5e4) * 0.0013
           + min(max(j - 1e5, 0), 9e5) * 0.0012 + max(j - 1e6, 0) * 0.0010)
    tht = min(t, 1e4) * 0.0164 + min(max(t - 1e4, 0), 2e4) * 0.015 + max(t - 3e4, 0) * 0.012
    return 25.56 + 8.21 + PART_TYPES * 1.53 + smt + tht + 3.58


def compute():
    rows = _bom_rows()
    out = []
    for n in SCALES:
        boards = 2 if n == 1 else n           # JLCPCB assembles at least 2
        c = dict(n=n, boards=boards)
        c["parts"] = _components(rows, boards, n)
        c["pcb"] = 2.0 if n == 1 else PCB_EACH[n] * n
        c["asm"] = _assembly(boards)
        c["offb"] = (BATTERY[n] + MOTOR[n] + ENCL[n] + PACK[n]) * n
        c["freight"] = FREIGHT[n]
        cif = c["parts"] + c["pcb"] + c["asm"] + c["offb"] + c["freight"]
        c["tax"] = IMPORT_TAX * cif
        c["labour"] = LABOUR_MIN[n] / 60 * LABOUR_USD_H * n + FIXTURE[n]
        base = cif + c["tax"] + c["labour"]
        c["total"] = base / YIELD[n]
        c["scrap"] = c["total"] - base
        c["unit"] = c["total"] / n
        c["parts_per_board"] = c["parts"] / boards
        out.append(c)
    return out


if __name__ == "__main__":
    for c in compute():
        print(f"{c['n']:>6}: ${c['unit']:.2f}  LKR {c['unit']*LKR_PER_USD:,.0f}  total ${c['total']:,.0f}")

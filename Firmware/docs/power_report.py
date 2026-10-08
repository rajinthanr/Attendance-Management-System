#!/usr/bin/env python3
"""Power, capacity and manufacturing cost report for the RFID attendance logger.

Writes an HTML page; print it to PDF with headless Chrome:

    python3 power_report.py /tmp/report.html
    google-chrome --headless=new --no-pdf-header-footer \
        --print-to-pdf=Power_Estimation_Report.pdf file:///tmp/report.html

Every figure is computed from the parameters below, so replace an estimate
with a measured value and regenerate. The currents were measured on the
bench on 8 October 2026 with the power-test build (`make power-test`,
Bsp/Src/bsp_power_test.c), which holds each load steady for a multimeter.
"""
import math
import os
import re
import sys
from datetime import date

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cost_model as cm   # noqa: E402  (reads PCB/bom/Attendance_Management_System_BOM_LCSC.xlsx)

OUT_HTML = sys.argv[1]

# --------------------------------------------------------------------------
# Parameters
# --------------------------------------------------------------------------
BATT_MAH = 1000.0
BATT_V = 3.7
HOURS_PER_MONTH = 730.0
WEEKDAYS_PER_MONTH = 52.14 * 5 / 12   # 21.7

# Battery-side currents. mA unless the key says uA. TYP holds the bench
# measurements (power-test build modes 1-10, 8 October 2026, multimeter in
# series with the cell); PES bounds what the meter could not hold steady and
# what is still an estimate.
TYP = dict(
    name="Measured",
    stop2_base_ua=3.3,   # mode 1: MCU Stop 2 + RTC + LPTIM1, reader power-down, board static
    off_ua=1.8,          # mode 10, Standby, with the SPI/LED pins pulled down (4.8 before)
    nfc_wu_ua=184.7,     # mode 2 minus mode 1: the meter read 117-204 uA, 188 averaged
    nfc_ready=5.66,      # mode 3
    field=203.0,         # mode 4, antenna not yet tuned
    run4=1.01,           # mode 5
    sleep4=0.72,         # mode 6
    led_g=2.77, led_r=3.0,  # modes 7, 8
    motor=56.5,          # mode 9
    run24=2.8, run80=9.0, adc_extra=0.2, flash_op=7.0, button=0.41,   # datasheet / estimate
    false_per_min=1.0, self_dis_pct=2.0, usable=0.90,
    poll_ms=10.0, empty_poll_ms=6.0,
)
PES = dict(TYP, name="Pessimistic",
           nfc_wu_ua=200.7, false_per_min=4.0, self_dis_pct=3.0, usable=0.85,
           poll_ms=14.0, empty_poll_ms=8.0)
MEASURED_ON = "8 October 2026"

# Firmware constants (App/Inc/app_config.h, bsp_board.h, feedback.c)
HB_ON_MS, HB_PERIOD_MS = 10, 5000
WAKES_PER_S, WAKE_MS = 2 / 5.0, 0.5     # heartbeat on and off edges; APP_SLEEP_MAX_MS = 5 s
BATT_SAMPLE_S, BATT_SAMPLE_MS = 10.0, 1.5
FB_ACCEPT_VIB, FB_ACCEPT_LED = 60, 250
FB_DUP_PULSE, FB_DUP_GAP = 60, 90
FB_ON_VIB, FB_ON_LED = 120, 300
FB_OFF_VIB, FB_OFF_LED = 250, 700
FB_HOLD_VIB = 60
FB_LECT_PULSE, FB_LECT_GAP = 80, 120
EMPTY_POLLS = 1                       # APP_NFC_REMOVE_MISSES
POLL_MS = 100
WU_PERIOD_MS = 100
PAGE_ERASE_MS = 22.0

# Flash: STM32L432KB, 128 kB = 64 pages of 2 kB (App/Inc/nv_layout.h,
# STM32L432KBUX_FLASH.ld). Code gets 36 pages, data 28 (config + 27 log).
FLASH_KB, PAGE_KB = 128, 2
CODE_PAGES, DATA_PAGES = 36, 28
LOG_PAGES, RECS_PER_PAGE = DATA_PAGES - 1, 254
LOG_CAP = LOG_PAGES * RECS_PER_PAGE  # 6858
MARKER_RECS = 6                      # typical lecture marker
ENDURANCE = 10000                    # STM32L4 flash cycles

# Measured image sizes (text + data, bytes), arm-none-eabi-gcc 13.3, 2026-10-07
IMG_O0, IMG_OG, IMG_OS, IMG_OS_LTO = 99244, 62052, 51496, 46220   # -Og/-Os 2026-10-08
OS_BREAKDOWN = [("Application logic (App/)", 19775), ("ST HAL drivers", 18532),
                ("Board support (Bsp/)", 6051), ("USB device library", 5598),
                ("Startup and CubeMX init (Core/)", 1390), ("C library", 184)]


def uah(uc):
    return uc / 3600.0


# --------------------------------------------------------------------------
# Model
# --------------------------------------------------------------------------
def idle_parts(p):
    return [
        ("ST25R3916 wake-up mode (100 ms)", p["nfc_wu_ua"]),
        ("Heartbeat LED (10 ms every 5 s)", p["led_g"] * 1000 * HB_ON_MS / HB_PERIOD_MS),
        ("MCU Stop 2, reader off, board static", p["stop2_base_ua"]),
        ("MCU loop wake-ups (0.4/s)", WAKES_PER_S * WAKE_MS / 1000 * p["run4"] * 1000),
        ("Battery sample (every 10 s)", BATT_SAMPLE_MS / 1000 * (p["run4"] + p["adc_extra"]) * 1000 / BATT_SAMPLE_S),
    ]


def off_parts(p):
    return [("Standby, reader power-down, board static (measured)", p["off_ua"])]


# Ready-mode time after a read: the pattern (the reader pauses), then the
# empty polls; the last poll's field time is counted separately.
READY_AFTER_TAP_MS = FB_ACCEPT_LED + (EMPTY_POLLS - 1) * POLL_MS


def tap_parts(p):
    """Charge (uC) of one accepted card tap."""
    return [
        (f"Vibration motor, {FB_ACCEPT_VIB} ms", p["motor"] * FB_ACCEPT_VIB),
        (f"Reader in Ready mode, {READY_AFTER_TAP_MS} ms", p["nfc_ready"] * READY_AFTER_TAP_MS),
        (f"{EMPTY_POLLS} empty poll before re-arming", EMPTY_POLLS * p["field"] * p["empty_poll_ms"]),
        ("RF field for the read, 10 ms", p["field"] * p["poll_ms"]),
        ("Green LED, 250 ms", p["led_g"] * FB_ACCEPT_LED),
        ("Re-arm wake-up (amplitude measure)", 100.0),
        ("MCU active during exchange, 15 ms", p["run4"] * 15),
        ("Oscillator start on wake-up, 1 ms", p["nfc_ready"] * 1),
        ("Flash write + share of page erase", 0.1 * p["flash_op"] + PAGE_ERASE_MS * p["flash_op"] / RECS_PER_PAGE),
    ]


def events(p):
    tap = sum(v for _, v in tap_parts(p))
    common_poll = (p["field"] * p["poll_ms"] + p["run4"] * 15 + p["nfc_ready"] * 1 + 100
                   + EMPTY_POLLS * p["field"] * p["empty_poll_ms"])
    dup = (common_poll + p["motor"] * 2 * FB_DUP_PULSE + p["led_g"] * 2 * FB_DUP_PULSE
           + p["nfc_ready"] * (2 * FB_DUP_PULSE + FB_DUP_GAP + (EMPTY_POLLS - 1) * POLL_MS))
    false_wake = (p["nfc_ready"] * (1 + (EMPTY_POLLS - 1) * POLL_MS) + EMPTY_POLLS * p["field"] * p["empty_poll_ms"]
                  + 100 + p["run4"] * 1.5)
    power_on = (20 * p["run80"] + 30 * p["run4"] + 5 * p["nfc_ready"] + 300 * p["button"]
                + p["motor"] * FB_ON_VIB + p["led_g"] * FB_ON_LED)
    auto_off = p["motor"] * FB_OFF_VIB + p["led_r"] * FB_OFF_LED + 2.0
    button_off = auto_off + 5000 * p["button"] + p["motor"] * FB_HOLD_VIB
    lect_led = 3 * FB_LECT_PULSE + 2 * FB_LECT_GAP + 400
    new_lecture = (3000 * p["button"] + p["motor"] * FB_HOLD_VIB + 10
                   + p["motor"] * 3 * FB_LECT_PULSE + p["led_g"] * lect_led)
    short_press = 200 * p["button"] + p["led_g"] * 2 * 150
    batt_sample = BATT_SAMPLE_MS * (p["run4"] + p["adc_extra"])
    page_erase = PAGE_ERASE_MS * p["flash_op"]
    return dict(tap=tap, dup=dup, false_wake=false_wake, power_on=power_on, auto_off=auto_off,
                button_off=button_off, new_lecture=new_lecture, short_press=short_press,
                batt_sample=batt_sample, page_erase=page_erase)


def lecture(p, n=100, dup_frac=0.05, tap_min=15.0, idle_tail_min=3.0):
    ev = events(p)
    idle_ua = sum(v for _, v in idle_parts(p))
    on_min = tap_min + idle_tail_min
    parts = [
        (f"{n} accepted taps", n * uah(ev["tap"])),
        (f"{round(n * dup_frac)} duplicate taps", round(n * dup_frac) * uah(ev["dup"])),
        (f"False wake-ups ({p['false_per_min']:g}/min)", p["false_per_min"] * on_min * uah(ev["false_wake"])),
        (f"Idle scanning, {on_min:g} min", idle_ua * on_min / 60.0),
        ("Lecture start (button hold)", uah(ev["new_lecture"])),
        ("Auto power-off pattern", uah(ev["auto_off"])),
        ("Power-on", uah(ev["power_on"])),
    ]
    return parts, sum(v for _, v in parts)


def monthly(p, lect_per_day, n=100, self_dis=True):
    _, q_lect = lecture(p, n)
    lect_month = lect_per_day * WEEKDAYS_PER_MONTH
    off_ua = sum(v for _, v in off_parts(p))
    dev = lect_month * q_lect / 1000.0 + off_ua * HOURS_PER_MONTH / 1000.0
    sd = BATT_MAH * p["self_dis_pct"] / 100.0 if self_dis else 0.0
    return dev, sd, dev + sd


def life_months(p, lect_per_day, n=100, self_dis=True):
    return BATT_MAH * p["usable"] / monthly(p, lect_per_day, n, self_dis)[2]


T, P = TYP, PES
ev_t, ev_p = events(T), events(P)
idle_t = sum(v for _, v in idle_parts(T))
idle_p = sum(v for _, v in idle_parts(P))
off_t = sum(v for _, v in off_parts(T))
off_p = sum(v for _, v in off_parts(P))
lect_parts_t, lect_t = lecture(T)
usable_t = BATT_MAH * T["usable"]
usable_p = BATT_MAH * P["usable"]

life4_t = life_months(T, 4)
life4_p = life_months(P, 4)
life4_t_nosd = life_months(T, 4, self_dis=False)
dev_m, sd_m, tot_m = monthly(T, 4)

poll_mode_ma = T["nfc_ready"] + T["field"] * T["empty_poll_ms"] / 100 + T["run4"] * 0.15 + idle_t / 1000
idle_life_h = usable_t / (idle_t / 1000)
poll_life_h = usable_t / poll_mode_ma
taps_bound = usable_t * 1000 / uah(ev_t["tap"])

LECT100 = LOG_CAP // (100 + MARKER_RECS)
CODE_BYTES = CODE_PAGES * PAGE_KB * 1024


# --------------------------------------------------------------------------
# Formatting helpers
# --------------------------------------------------------------------------
def f(x, d=1):
    return f"{x:,.{d}f}"


def qfmt(uc):
    return f"{uc/1000:,.2f} mC" if uc >= 100 else f"{uc:,.1f} uC"


def bar_path(x, y, w, h, color, r=4):
    r = min(r, w / 2, h / 2)
    return (f'<path d="M{x:.1f},{y:.1f} H{x+w-r:.1f} Q{x+w:.1f},{y:.1f} {x+w:.1f},{y+r:.1f} '
            f'V{y+h-r:.1f} Q{x+w:.1f},{y+h:.1f} {x+w-r:.1f},{y+h:.1f} H{x:.1f} Z" fill="{color}"/>')


def nice_ticks(max_v, n=5):
    raw = max_v / n
    mag = 10 ** math.floor(math.log10(raw))
    step = min((s * mag for s in (1, 2, 2.5, 5, 10) if s * mag >= raw))
    return [i * step for i in range(int(max_v / step) + 1)]


def fmt_tick(t):
    if t >= 1000:
        return f"{t:,.0f}"
    if t == int(t):
        return f"{int(t)}"
    return f"{t:g}"


def svg_hbar(items, color, unit, width=680, row=30, label_w=250, fmt=lambda v: f(v, 1),
             max_v=None, ref=None, marker=None):
    max_v = max_v or max([v for _, v in items] + ([marker[1]] if marker else [])) * 1.18
    plot_w = width - label_w - 70
    h = row * len(items) + 34
    out = [f'<svg viewBox="0 0 {width} {h}" class="chart" role="img">']
    for t in nice_ticks(max_v):
        x = label_w + plot_w * t / max_v
        out.append(f'<line x1="{x:.1f}" y1="4" x2="{x:.1f}" y2="{h-26}" class="grid"/>')
        out.append(f'<text x="{x:.1f}" y="{h-10}" class="tick" text-anchor="middle">{fmt_tick(t)}</text>')
    out.append(f'<text x="{width-4}" y="{h-10}" class="tick" text-anchor="end">{unit}</text>')
    for i, (lab, v) in enumerate(items):
        y = 6 + i * row
        bw = max(plot_w * v / max_v, 1.5)
        c = color if (ref is None or lab != ref) else "#a3a29d"
        out.append(f'<text x="{label_w-10}" y="{y+row/2+1}" class="lab" text-anchor="end" dominant-baseline="middle">{lab}</text>')
        out.append(bar_path(label_w, y + 5, bw, row - 12, c))
        out.append(f'<text x="{label_w+bw+6:.1f}" y="{y+row/2+1}" class="val" dominant-baseline="middle">{fmt(v)}</text>')
    if marker:
        x = label_w + plot_w * marker[1] / max_v
        out.append(f'<line x1="{x:.1f}" y1="2" x2="{x:.1f}" y2="{h-26}" stroke="#eb6834" stroke-width="1.5" stroke-dasharray="4 3"/>')
        out.append(f'<text x="{x-4:.1f}" y="{h-30}" class="tick" text-anchor="end" fill="#b84a1d">{marker[0]}</text>')
    out.append(f'<line x1="{label_w}" y1="4" x2="{label_w}" y2="{h-26}" class="axis"/>')
    out.append("</svg>")
    return "\n".join(out)


def svg_life_chart():
    xs = list(range(1, 11))
    s1 = [life_months(T, x, self_dis=False) for x in xs]
    s2 = [life_months(T, x, self_dis=True) for x in xs]
    width, height = 680, 300
    l, r, t, b = 52, 20, 34, 40
    pw, ph = width - l - r, height - t - b
    ymax = int(math.ceil(max(s1) / 20.0) * 20)
    X = lambda x: l + pw * (x - 1) / 9
    Y = lambda y: t + ph * (1 - y / ymax)
    out = [f'<svg viewBox="0 0 {width} {height}" class="chart" role="img">']
    for yv in range(0, ymax + 1, 20):
        out.append(f'<line x1="{l}" y1="{Y(yv):.1f}" x2="{l+pw}" y2="{Y(yv):.1f}" class="grid"/>')
        out.append(f'<text x="{l-8}" y="{Y(yv)+4:.1f}" class="tick" text-anchor="end">{yv}</text>')
    for x in xs:
        out.append(f'<text x="{X(x):.1f}" y="{t+ph+18}" class="tick" text-anchor="middle">{x}</text>')
    out.append(f'<text x="{l+pw/2}" y="{height-4}" class="tick" text-anchor="middle">Lectures per weekday (100 students each)</text>')
    out.append(f'<text x="14" y="{t+ph/2}" class="tick" text-anchor="middle" transform="rotate(-90 14 {t+ph/2})">Months per charge</text>')
    out.append(f'<line x1="{l}" y1="{t+ph}" x2="{l+pw}" y2="{t+ph}" class="axis"/>')
    out.append(f'<line x1="{X(4):.1f}" y1="{t}" x2="{X(4):.1f}" y2="{t+ph}" stroke="#c3c2b7" stroke-dasharray="3 3"/>')
    out.append(f'<text x="{X(4)+5:.1f}" y="{t+10}" class="tick">design point</text>')
    series = ((s1, "#2a78d6", "Device only"), (s2, "#eb6834", "Including 2 %/month cell self-discharge"))
    lx = l + 4
    for ser, col, name in series:
        pts = " ".join(f"{X(x):.1f},{Y(y):.1f}" for x, y in zip(xs, ser))
        out.append(f'<polyline points="{pts}" fill="none" stroke="{col}" stroke-width="2"/>')
        for x, y in zip(xs, ser):
            out.append(f'<circle cx="{X(x):.1f}" cy="{Y(y):.1f}" r="4" fill="{col}" stroke="#ffffff" stroke-width="2"/>')
        out.append(f'<line x1="{lx}" y1="10" x2="{lx+18}" y2="10" stroke="{col}" stroke-width="2"/>')
        out.append(f'<circle cx="{lx+9}" cy="10" r="3.5" fill="{col}"/>')
        out.append(f'<text x="{lx+24}" y="13.5" class="lab">{name}</text>')
        lx += 24 + 7.2 * len(name) + 18
    out.append(f'<text x="{X(4)+8:.1f}" y="{Y(s1[3])-8:.1f}" class="val">{s1[3]:.0f} months</text>')
    out.append(f'<text x="{X(4)+8:.1f}" y="{Y(s2[3])+16:.1f}" class="val">{s2[3]:.0f} months</text>')
    out.append(f'<text x="{X(1)+8:.1f}" y="{Y(s1[0])+4:.1f}" class="val">{s1[0]:.0f}</text>')
    out.append(f'<text x="{X(1)+8:.1f}" y="{Y(s2[0])-8:.1f}" class="val">{s2[0]:.0f}</text>')
    out.append("</svg>")
    return "\n".join(out)


def svg_flash_chart(sizes):
    vals = [LOG_CAP // (n + MARKER_RECS) for n in sizes]
    width, height = 680, 250
    l, r, t, b = 52, 16, 22, 40
    pw, ph = width - l - r, height - t - b
    ymax = int(math.ceil(max(vals) / 50.0) * 50)
    slot = pw / len(sizes)
    bw = slot * 0.56
    Y = lambda y: t + ph * (1 - y / ymax)
    out = [f'<svg viewBox="0 0 {width} {height}" class="chart" role="img">']
    for yv in range(0, ymax + 1, 50):
        out.append(f'<line x1="{l}" y1="{Y(yv):.1f}" x2="{l+pw}" y2="{Y(yv):.1f}" class="grid"/>')
        out.append(f'<text x="{l-8}" y="{Y(yv)+4:.1f}" class="tick" text-anchor="end">{yv}</text>')
    for i, (n, v) in enumerate(zip(sizes, vals)):
        x = l + slot * i + (slot - bw) / 2
        y = Y(v)
        rr = 4
        out.append(f'<path d="M{x:.1f},{t+ph:.1f} V{y+rr:.1f} Q{x:.1f},{y:.1f} {x+rr:.1f},{y:.1f} H{x+bw-rr:.1f} '
                   f'Q{x+bw:.1f},{y:.1f} {x+bw:.1f},{y+rr:.1f} V{t+ph:.1f} Z" fill="#2a78d6"/>')
        out.append(f'<text x="{x+bw/2:.1f}" y="{y-6:.1f}" class="val" text-anchor="middle">{v}</text>')
        out.append(f'<text x="{x+bw/2:.1f}" y="{t+ph+18}" class="tick" text-anchor="middle">{n}</text>')
    out.append(f'<line x1="{l}" y1="{t+ph}" x2="{l+pw}" y2="{t+ph}" class="axis"/>')
    out.append(f'<text x="{l+pw/2}" y="{height-4}" class="tick" text-anchor="middle">Students per lecture</text>')
    out.append(f'<text x="14" y="{t+ph/2}" class="tick" text-anchor="middle" transform="rotate(-90 14 {t+ph/2})">Lectures until full</text>')
    out.append("</svg>")
    return "\n".join(out)


def svg_flash_map():
    """The 64 pages as a strip: code, free code space, config, log."""
    width, height = 680, 92
    l, r = 4, 4
    pw = width - l - r
    pg = pw / 64
    used_pages = IMG_OG / 2048
    out = [f'<svg viewBox="0 0 {width} {height}" class="chart" role="img">']
    for i in range(64):
        x = l + i * pg
        if i < CODE_PAGES:
            c = "#2a78d6" if i + 1 <= used_pages else ("#9cc0ec" if i < used_pages else "#e3edf9")
        elif i == CODE_PAGES:
            c = "#eda100"
        else:
            c = "#1baf7a"
        out.append(f'<rect x="{x+0.6:.1f}" y="26" width="{pg-1.2:.1f}" height="26" rx="1.5" fill="{c}"/>')
    def brace(p0, p1, text, sub, anchor="middle"):
        x0, x1 = l + p0 * pg, l + p1 * pg
        out.append(f'<line x1="{x0+1:.1f}" y1="20" x2="{x1-1:.1f}" y2="20" stroke="#8a8984"/>')
        out.append(f'<text x="{(x0+x1)/2:.1f}" y="14" class="lab" text-anchor="middle">{text}</text>')
        out.append(f'<text x="{(x0+x1)/2:.1f}" y="70" class="tick" text-anchor="middle">{sub}</text>')
    brace(0, CODE_PAGES, f"Code: {CODE_PAGES} pages, {CODE_PAGES*2} kB",
          f"0x08000000 · image {IMG_OG/1024:.1f} kB at -Og, {(CODE_BYTES-IMG_OG)/1024:.1f} kB free")
    brace(CODE_PAGES + 1, 64, f"Attendance log: {LOG_PAGES} pages, {LOG_CAP:,} records", "0x08012800 · 254 records per page")
    xcfg = l + (CODE_PAGES + 0.5) * pg
    out.append(f'<text x="{xcfg:.1f}" y="86" class="tick" text-anchor="middle">config page 0x08012000</text>')
    out.append(f'<line x1="{xcfg:.1f}" y1="53" x2="{xcfg:.1f}" y2="77" stroke="#eda100"/>')
    out.append("</svg>")
    return "\n".join(out)


# Timeline colours: one per state, shared by the three timeline figures.
C_STOP2, C_RUN = "#e4e3dd", "#2a78d6"
C_WU, C_MEAS, C_READY, C_FIELD = "#f8dccf", "#eb6834", "#eda100", "#c8372d"
C_LED, C_MOTOR = "#1baf7a", "#7d5bd6"


def svg_timeline(t0, t1, lanes, ticks, tick_fmt, unit, markers=(), width=680, label_w=112, lane_h=34):
    """Swimlanes against time.

    lanes:   [(name, sub, segments, points)]; segments are (start, end, colour,
             text, text colour), drawn at least 1.6 px wide, with the text inside
             when it fits and to the right otherwise; points are (t, colour).
    markers: [(t, text, anchor)] dashed lines labelled above the lanes.
    """
    plot_w = width - label_w - 14
    top = 30 if markers else 8
    h = top + lane_h * len(lanes) + 30
    X = lambda t: label_w + plot_w * (t - t0) / (t1 - t0)
    bottom = top + lane_h * len(lanes)
    out = [f'<svg viewBox="0 0 {width} {h}" class="chart" role="img">']
    for t in ticks:
        out.append(f'<line x1="{X(t):.1f}" y1="{top}" x2="{X(t):.1f}" y2="{bottom}" class="grid"/>')
        out.append(f'<text x="{X(t):.1f}" y="{bottom+14}" class="tick" text-anchor="middle">{tick_fmt(t)}</text>')
    out.append(f'<text x="{width-2}" y="{bottom+27}" class="tick" text-anchor="end">{unit}</text>')
    for i, (name, sub, segs, pts) in enumerate(lanes):
        y = top + i * lane_h
        out.append(f'<text x="{label_w-10}" y="{y+lane_h/2-3}" class="lab" text-anchor="end">{name}</text>')
        out.append(f'<text x="{label_w-10}" y="{y+lane_h/2+9}" class="tick" text-anchor="end">{sub}</text>')
        out.append(f'<rect x="{label_w}" y="{y+5}" width="{plot_w}" height="{lane_h-10}" fill="#f7f6f2"/>')
        for a, b, col, text, tcol in segs:
            x0, x1 = X(max(a, t0)), X(min(b, t1))
            w = max(x1 - x0, 1.6)
            out.append(f'<rect x="{x0:.1f}" y="{y+5}" width="{w:.1f}" height="{lane_h-10}" rx="1.5" fill="{col}"/>')
            if text:
                if w > 5.3 * len(text) + 8:
                    out.append(f'<text x="{x0+w/2:.1f}" y="{y+lane_h/2+3.5}" class="seg" text-anchor="middle" '
                               f'fill="{tcol}">{text}</text>')
                else:
                    out.append(f'<text x="{x0+w+4:.1f}" y="{y+lane_h/2+3.5}" class="seg" fill="#0b0b0b">{text}</text>')
        for t, col in pts:
            if not t0 <= t <= t1:
                continue
            out.append(f'<rect x="{X(t)-0.9:.1f}" y="{y+5}" width="1.8" height="{lane_h-10}" fill="{col}"/>')
    for t, text, anchor in markers:
        x = X(t)
        out.append(f'<line x1="{x:.1f}" y1="{top-6}" x2="{x:.1f}" y2="{bottom}" stroke="#52514e" '
                   f'stroke-width="0.8" stroke-dasharray="3 2"/>')
        dx = {"start": 3, "end": -3}.get(anchor, 0)
        out.append(f'<text x="{x+dx:.1f}" y="{top-10}" class="tick" text-anchor="{anchor}">{text}</text>')
    out.append(f'<line x1="{label_w}" y1="{bottom}" x2="{label_w+plot_w}" y2="{bottom}" class="axis"/>')
    out.append("</svg>")
    return "\n".join(out)


def svg_idle_timeline(p):
    """Ten seconds of idle scanning: who is awake, and when."""
    t1 = 10.4
    wakes = [0.0, 0.010, 5.0, 5.010, 10.0, 10.010]
    meas = [i * WU_PERIOD_MS / 1000 for i in range(int(t1 * 1000 / WU_PERIOD_MS) + 1)]
    lanes = [
        ("MCU", f"run {p['run4']:.2f} mA",
         [(0, 5.0, C_STOP2, f"Stop 2, {p['stop2_base_ua']} uA with the board's static draw", "#52514e"),
          (5.0, t1, C_STOP2, "", "")],
         [(t, C_RUN) for t in wakes]),
        ("ST25R3916", f"{p['nfc_wu_ua']:.0f} uA average",
         [(0, t1, C_WU, "", "")], [(t, C_MEAS) for t in meas]),
        ("Green LED", f"{p['led_g']:.2f} mA", [], [(0.0, C_LED), (5.0, C_LED), (10.0, C_LED)]),
        ("RF field", "off while idle", [], []),
    ]
    markers = [(0.0, "heartbeat + battery sample", "start"), (5.0, "heartbeat", "middle"),
               (10.0, "heartbeat + battery sample", "end")]
    return svg_timeline(-0.15, t1, lanes, list(range(0, 11)), lambda t: f"{t}", "seconds", markers)


# One wake-up measurement, illustrated. Its charge follows from the measured
# average; the split between oscillator start-up and the field burst is an
# assumption until the pulse is captured on a scope across a shunt.
WU_OSC_MS = 1.0                            # assumed: 27.12 MHz oscillator start, at Ready current
WU_BASE_UA = 5.0                           # drawn: board 3.3 uA + the reader's wake-up timer


def wu_pulse(p):
    q = p["nfc_wu_ua"] * WU_PERIOD_MS / 1000   # uC per measurement
    t_field = (q - p["nfc_ready"] * WU_OSC_MS) / p["field"]   # ms
    return q, t_field


def svg_wakeup_pulses(p):
    """Battery current through four wake-up measurements, and one zoomed in."""
    q, t_f = wu_pulse(p)
    width, label_w = 680, 112
    plot_w = width - label_w - 14
    lo, hi = 1.0, 3e5                          # uA, log axis
    top_t0, top_t1 = -20.0, 345.0
    z_t0, z_t1 = -0.2, 1.45
    p1_top, p1_h = 26, 120
    p2_top, p2_h = 228, 120
    h = p2_top + p2_h + 34

    def Y(ua, top, ph):
        return top + ph * (1 - (math.log10(max(ua, lo)) - math.log10(lo)) / (math.log10(hi) - math.log10(lo)))

    def X(t, a, b):
        return label_w + plot_w * (t - a) / (b - a)

    out = [f'<svg viewBox="0 0 {width} {h}" class="chart" role="img">']

    def axis(top, ph, a, b, ticks, unit, title):
        for ua, lab in ((1, "1 uA"), (10, "10 uA"), (100, "100 uA"), (1e3, "1 mA"), (1e4, "10 mA"), (1e5, "100 mA")):
            y = Y(ua, top, ph)
            out.append(f'<line x1="{label_w}" y1="{y:.1f}" x2="{label_w+plot_w}" y2="{y:.1f}" class="grid"/>')
            out.append(f'<text x="{label_w-6}" y="{y+3:.1f}" class="tick" text-anchor="end">{lab}</text>')
        for t in ticks:
            x = X(t, a, b)
            out.append(f'<line x1="{x:.1f}" y1="{top}" x2="{x:.1f}" y2="{top+ph}" class="grid"/>')
            out.append(f'<text x="{x:.1f}" y="{top+ph+13}" class="tick" text-anchor="middle">{t:g}</text>')
        out.append(f'<text x="{width-2}" y="{top+ph+26}" class="tick" text-anchor="end">{unit}</text>')
        out.append(f'<line x1="{label_w}" y1="{top+ph}" x2="{label_w+plot_w}" y2="{top+ph}" class="axis"/>')
        out.append(f'<text x="{label_w}" y="{top-12}" class="lab" font-weight="600">{title}</text>')
        out.append(f'<text x="{label_w-6}" y="{top-12}" class="tick" text-anchor="end">battery current</text>')

    def trace(steps, top, ph, a, b, min_w=0.0):
        """steps: [(t_start, t_end, uA)] above the baseline; drawn as a filled step line."""
        base = Y(WU_BASE_UA, top, ph)
        d = f"M{X(a, a, b):.1f},{base:.1f}"
        for t0, t1, ua in steps:
            x0, x1 = X(t0, a, b), X(t1, a, b)
            x1 = max(x1, x0 + min_w)
            y = Y(ua, top, ph)
            d += f" H{x0:.1f} V{y:.1f} H{x1:.1f} V{base:.1f}"
        d += f" H{X(b, a, b):.1f}"
        out.append(f'<path d="{d} V{top+ph:.1f} H{label_w} Z" fill="{C_WU}" stroke="none"/>')
        out.append(f'<path d="{d}" fill="none" stroke="{C_MEAS}" stroke-width="1.3"/>')

    # Panel 1: four measurements, the last one sees a card and the read follows.
    axis(p1_top, p1_h, top_t0, top_t1, [0, 50, 100, 150, 200, 250, 300], "ms", "Four measurements")
    meas = [0.0, 100.0, 200.0, 300.0]
    steps = []
    for t in meas:
        steps += [(t, t + WU_OSC_MS, p["nfc_ready"] * 1000), (t + WU_OSC_MS, t + WU_OSC_MS + t_f, p["field"] * 1000)]
    steps.append((302.0, 302.0 + p["poll_ms"], p["field"] * 1000))   # the read after the IRQ
    trace(steps, p1_top, p1_h, top_t0, top_t1, min_w=2.0)
    yb = Y(WU_BASE_UA, p1_top, p1_h)
    for a, b in ((0, 100), (100, 200), (200, 300)):
        xa, xb = X(a, top_t0, top_t1) + 4, X(b, top_t0, top_t1) - 4
        y = Y(3e4, p1_top, p1_h)
        out.append(f'<line x1="{xa:.1f}" y1="{y:.1f}" x2="{xb:.1f}" y2="{y:.1f}" stroke="#52514e" stroke-width="0.8" '
                   f'marker-start="url(#arr)" marker-end="url(#arr)"/>')
        out.append(f'<text x="{(xa+xb)/2:.1f}" y="{y-4:.1f}" class="tick" text-anchor="middle">{WU_PERIOD_MS} ms</text>')
    out.append(f'<text x="{X(50, top_t0, top_t1):.1f}" y="{yb-6:.1f}" class="tick" text-anchor="middle">a few uA between</text>')
    x4 = X(300, top_t0, top_t1)
    out.append(f'<text x="{x4-6:.1f}" y="{Y(2e2, p1_top, p1_h):.1f}" class="tick" text-anchor="end">card in range:</text>')
    out.append(f'<text x="{x4-6:.1f}" y="{Y(2e2, p1_top, p1_h)+10:.1f}" class="tick" text-anchor="end">IRQ, MCU wakes, read</text>')
    out.append('<defs><marker id="arr" viewBox="0 0 6 6" refX="3" refY="3" markerWidth="5" markerHeight="5" orient="auto-start-reverse">'
               '<path d="M0,0 L6,3 L0,6 z" fill="#52514e"/></marker></defs>')

    # Zoom guides from the second measurement down to panel 2.
    xz = X(100, top_t0, top_t1)
    out.append(f'<path d="M{xz-3:.1f},{p1_top+p1_h+17} L{label_w},{p2_top-22} M{xz+3:.1f},{p1_top+p1_h+17} L{label_w+plot_w},{p2_top-22}" '
               f'stroke="#a3a29d" stroke-width="0.7" stroke-dasharray="3 2" fill="none"/>')

    # Panel 2: one measurement.
    axis(p2_top, p2_h, z_t0, z_t1, [0, 0.25, 0.5, 0.75, 1.0, 1.25], "ms", "One measurement")
    trace([(0, WU_OSC_MS, p["nfc_ready"] * 1000), (WU_OSC_MS, WU_OSC_MS + t_f, p["field"] * 1000)],
          p2_top, p2_h, z_t0, z_t1)
    xa, xb = X(0, z_t0, z_t1), X(WU_OSC_MS, z_t0, z_t1)
    yr = Y(p["nfc_ready"] * 1000, p2_top, p2_h)
    out.append(f'<text x="{(xa+xb)/2:.1f}" y="{yr-6:.1f}" class="lab" text-anchor="middle">oscillator starts, Ready '
               f'{p["nfc_ready"]:.2f} mA (~{WU_OSC_MS:g} ms, assumed)</text>')
    xf = X(WU_OSC_MS + t_f, z_t0, z_t1)
    yf = Y(p["field"] * 1000, p2_top, p2_h)
    out.append(f'<text x="{xf+5:.1f}" y="{yf+4:.1f}" class="lab">field on, amplitude measured:</text>')
    out.append(f'<text x="{xf+5:.1f}" y="{yf+16:.1f}" class="lab">{p["field"]:.0f} mA for ~{t_f*1000:.0f} us</text>')
    out.append(f'<text x="{(xa+xb)/2:.1f}" y="{Y(400, p2_top, p2_h):.1f}" class="val" text-anchor="middle">{q:.1f} uC per measurement</text>')
    out.append(f'<text x="{(xa+xb)/2:.1f}" y="{Y(400, p2_top, p2_h)+12:.1f}" class="tick" text-anchor="middle">= {p["nfc_wu_ua"]:.0f} uA '
               f'average x {WU_PERIOD_MS} ms, measured</text>')
    out.append("</svg>")
    return "\n".join(out)


def svg_tap_timeline(p):
    """One accepted tap, from the wake-up measurement that sees the card."""
    rd = p["poll_ms"]                      # field on for the read: guard + select
    end_fb = rd + FB_ACCEPT_LED
    ep = p["empty_poll_ms"]
    lanes = [
        ("Card", "", [(-62, 40, "#d9d8d1", "held to the antenna", "#0b0b0b")], []),
        ("ST25R3916", "", [
            (-130, 0, C_WU, "wake-up mode", "#8a4a2d"),
            (0, 1, C_READY, "", ""),
            (1, rd, C_FIELD, "", ""),
            (rd, end_fb, C_READY, f"Ready, field off: {p['nfc_ready']:.2f} mA", "#0b0b0b"),
            (end_fb, end_fb + ep, C_FIELD, "", ""),
            (end_fb + ep + 1, 330, C_WU, "wake-up mode", "#8a4a2d"),
        ], [(-100, C_MEAS), (0, C_MEAS), (end_fb + ep + 100 + 1, C_MEAS)]),
        ("MCU", "", [
            (-130, 330, C_STOP2, "", ""),
            (0, 1, C_RUN, "", ""), (6, rd, C_RUN, "", ""),
            (rd + FB_ACCEPT_VIB, rd + FB_ACCEPT_VIB + 0.3, C_RUN, "", ""),
            (end_fb, end_fb + 0.5, C_RUN, "", ""), (end_fb + ep, end_fb + ep + 1, C_RUN, "", ""),
        ], []),
        ("Motor", f"{p['motor']:.1f} mA", [(rd, rd + FB_ACCEPT_VIB, C_MOTOR, f"{FB_ACCEPT_VIB} ms", "#ffffff")], []),
        ("Green LED", f"{p['led_g']:.2f} mA", [(rd, end_fb, C_LED, f"{FB_ACCEPT_LED} ms", "#ffffff")], []),
    ]
    markers = [(0, "card seen: IRQ", "end"), (rd, f"read done ({rd:.0f} ms of field)", "start"),
               (end_fb, f"empty poll ({ep:.0f} ms), re-arm", "end")]
    return svg_timeline(-130, 330, lanes, list(range(-100, 301, 50)), lambda t: f"{t}", "ms after the card is seen",
                        markers)


def svg_false_timeline(p):
    """A wake-up measurement that strays with no card: one empty poll."""
    ep = p["empty_poll_ms"]
    lanes = [
        ("ST25R3916", "", [(-6, 0, C_WU, "wake-up", "#8a4a2d"), (0, 1, C_READY, "", ""),
                           (1, 1 + ep, C_FIELD, f"field {ep:.0f} ms, no answer", "#ffffff"),
                           (1 + ep, 2 + ep, C_READY, "", ""), (2 + ep, 16, C_WU, "wake-up mode again", "#8a4a2d")],
         [(0, C_MEAS)]),
        ("MCU", "", [(-6, 16, C_STOP2, "", ""), (0, 1, C_RUN, "", ""), (1 + ep, 2 + ep, C_RUN, "", "")], []),
    ]
    markers = [(0, "IRQ", "middle"), (2 + ep, "measure + re-arm", "start")]
    return svg_timeline(-6, 16, lanes, list(range(-5, 16, 5)), lambda t: f"{t}", "ms", markers, lane_h=32)


def table(head, rows, cls="", align=None):
    align = align or ["l"] + ["r"] * (len(head) - 1)
    h = "".join(f'<th class="{a}">{c}</th>' for c, a in zip(head, align))
    body = ""
    for r in rows:
        tr_cls = ""
        if isinstance(r, tuple) and len(r) == 2 and isinstance(r[1], str) and r[1].startswith("cls:"):
            r, tr_cls = r[0], r[1][4:]
        body += f'<tr class="{tr_cls}">' + "".join(f'<td class="{a}">{c}</td>' for c, a in zip(r, align)) + "</tr>"
    return f'<table class="{cls}"><thead><tr>{h}</tr></thead><tbody>{body}</tbody></table>'


def eqs(rows):
    w1 = max(len(a) for a, _, _ in rows)
    w2 = max(len(b) for _, b, _ in rows)
    return "\n".join(f"{a.ljust(w1)}  =  {b.ljust(w2)}  =  {c}" for a, b, c in rows)


# --------------------------------------------------------------------------
# Tables and figures
# --------------------------------------------------------------------------
today = date.today().strftime("%d %B %Y")

idle_items = sorted(idle_parts(T), key=lambda kv: -kv[1])
tap_items = sorted([(k, v / 1000) for k, v in tap_parts(T)], key=lambda kv: -kv[1])

clock_items = [
    ("MCU at 4 MHz (selected)", T["run4"] * 15),
    ("MCU at 24 MHz", T["run24"] * 15),
    ("MCU at 80 MHz (PLL)", T["run80"] * 15),
    ("RF field for the same poll", T["field"] * T["poll_ms"]),
]

event_rows = [
    ("Accepted card tap", f"Field 10 ms, motor {FB_ACCEPT_VIB} ms, LED 250 ms, Ready {READY_AFTER_TAP_MS} ms, 1 empty poll", ev_t["tap"], ev_p["tap"]),
    ("Duplicate tap", "Same read, 2 x 60 ms motor + LED", ev_t["dup"], ev_p["dup"]),
    ("False wake-up", "Oscillator start + 1 empty poll (6 ms field), no feedback", ev_t["false_wake"], ev_p["false_wake"]),
    ("Power-on (button)", "Boot (20 ms at 80 MHz), reader start, 120 ms motor, 300 ms LED", ev_t["power_on"], ev_p["power_on"]),
    ("Lecture start (2-5 s hold)", "3 s hold, 60 ms + 3 x 80 ms motor, 880 ms LED, marker write", ev_t["new_lecture"], ev_p["new_lecture"]),
    ("Auto power-off (3 min idle)", "250 ms motor, 700 ms red LED, flush, reader power-down", ev_t["auto_off"], ev_p["auto_off"]),
    ("Power-off (5 s hold)", "As above + 5 s hold + 60 ms hold buzz", ev_t["button_off"], ev_p["button_off"]),
    ("Short press (battery check)", "0.2 s press, two 150 ms green blinks", ev_t["short_press"], ev_p["short_press"]),
    ("Flash page erase", "22 ms at ~7 mA, once per 254 records", ev_t["page_erase"], ev_p["page_erase"]),
    ("Battery sample", "1.5 ms MCU + ADC, every 10 s, on a heartbeat wake", ev_t["batt_sample"], ev_p["batt_sample"]),
]


def uah_cell(v):
    return f(uah(v), 2) if uah(v) >= 0.01 else f"{uah(v):.4f}"


event_table = table(
    ["Event", "What draws current", "Charge (typ.)", "uAh (typ.)", "uAh (pess.)"],
    [(e, d, qfmt(t), uah_cell(t), uah_cell(pv)) for e, d, t, pv in event_rows],
    align=["l", "l", "r", "r", "r"], cls="events")

mode_rows = [
    ("Off (Standby)", f"{off_t:.1f} uA", f"{off_p:.1f} uA", "MCU Standby + RTC, reader power-down, divider (measured)"),
    ("Idle, scanning", f"{idle_t:.0f} uA", f"{idle_p:.0f} uA", "Stop 2, reader wake-up mode, heartbeat"),
    ("Polling mode (wake-up off)", f"{poll_mode_ma:.1f} mA", "-", "Reader Ready + field every 100 ms; for comparison"),
    ("Card read (RF field on)", f"{T['field']:.0f} mA", "-", "For ~10 ms per read; antenna untuned (measured)"),
    ("Vibration motor on", f"{T['motor']:.1f} mA", "-", "60-250 ms per pattern (measured)"),
    ("USB session", "4-6 mA", "-", "From VBUS, not the battery (plus charge current)"),
]

lect_rows = [(k, f(v, 1), f"{100*v/lect_t:.0f} %") for k, v in lect_parts_t]
lect_rows.append((("<b>Total per lecture</b>", f"<b>{lect_t:.0f}</b>", "100 %"), "cls:total"))

scen_rows = []
for name, lpd, n in (("Light", 1, 50), ("Typical", 4, 100), ("Busy", 6, 150), ("Heavy", 8, 200)):
    _, ql = lecture(T, n)
    scen_rows.append((f"{name}: {lpd}/day x {n} students", f"{ql:.0f}",
                      f"{life_months(T, lpd, n, self_dis=False):.0f}", f"{life_months(T, lpd, n):.0f}",
                      f"{life_months(P, lpd, n):.0f}"))
scen_table = table(["Scenario (weekdays)", "uAh per lecture", "Months, device only", "Months, with self-discharge",
                    "Months, pessimistic"], scen_rows, cls="scen")

reads_rows, fills = [], []
for n in (30, 60, 100, 200):
    _, ql = lecture(T, n)
    per = ql / n
    taps = usable_t * 1000 / per
    fills.append(taps / LOG_CAP)
    reads_rows.append((f"{n} students per lecture", f"{per:.2f}", f"{taps:,.0f}", f"{taps/LOG_CAP:.0f}x"))
reads_table = table(["Usage pattern", "uAh per student (all overheads)", "Taps per charge (900 mAh)",
                     "Flash fills per charge"], reads_rows, cls="scen")

flash_sizes = [30, 50, 100, 150, 200, 300]
flash_rows = []
for n in flash_sizes:
    lect = LOG_CAP // (n + MARKER_RECS)
    days = lect / 4
    flash_rows.append((f"{n}", f"{n+MARKER_RECS}", f"{lect}", f"{lect*n:,}", f"{days:.0f} days ({days/5:.1f} weeks)"))
flash_table = table(["Students / lecture", "Records / lecture", "Lectures until full", "Student taps stored",
                     "At 4 lectures per weekday"], flash_rows, cls="keep")

image_rows = [
    ("CubeIDE Debug before this change (-O0)", f"{IMG_O0:,}", f"{IMG_O0/1024:.1f} kB", "No", "Larger than any split that leaves a useful log"),
    ("<b>make and CubeIDE Debug (-Og)</b>", f"<b>{IMG_OG:,}</b>", f"{(CODE_BYTES-IMG_OG)/1024:.1f} kB free",
     "Yes", f"{100*IMG_OG/CODE_BYTES:.0f} % of the code region; full debugging"),
    ("CubeIDE Release (-Os)", f"{IMG_OS:,}", f"{(CODE_BYTES-IMG_OS)/1024:.1f} kB free", "Yes", "Shipping build"),
    ("-Os with link-time optimisation", f"{IMG_OS_LTO:,}", f"{(CODE_BYTES-IMG_OS_LTO)/1024:.1f} kB free", "Yes",
     "Not used: hardest to debug"),
]
image_table = table(["Build", "Image (bytes)", "In 72 kB", "Fits", "Note"], image_rows,
                    align=["l", "r", "r", "l", "l"], cls="keep")

split_rows = []
for code_kb in (64, 72, 80):
    cp = code_kb // 2
    lp = 64 - cp - 1
    cap = lp * RECS_PER_PAGE
    head = (code_kb * 1024 - IMG_OG) / 1024
    chosen = code_kb == CODE_PAGES * 2
    row = (f"{code_kb} kB / {128-code_kb} kB" + (" (chosen)" if chosen else ""), f"{lp}", f"{cap:,}",
           f"{cap // (100 + MARKER_RECS)}", f"{head:.1f} kB", f"{(code_kb*1024-IMG_OS)/1024:.1f} kB")
    split_rows.append((tuple(f"<b>{c}</b>" for c in row), "cls:") if chosen else row)
split_table = table(["Code / data", "Log pages", "Records", "Lectures of 100", "Headroom at -Og", "Headroom at -Os"],
                    split_rows, cls="keep")

meas_rows = [
    ("1", "MCU Stop 2 + RTC + LPTIM1, reader power-down", f"{T['stop2_base_ua']} uA", "Includes the divider, LDO and charger leakage"),
    ("2", "As 1, reader in wake-up mode (100 ms)", f"{T['stop2_base_ua'] + T['nfc_wu_ua']:.0f} uA",
     "Meter read 117-204 uA as it averaged the 100 ms measurements; 188 averaged"),
    ("3", "As 1, reader Ready (oscillator on, field off)", f"{T['nfc_ready']} mA", "Reader between polls and during patterns"),
    ("4", "As 1, RF field on", f"{T['field']:.0f} mA", "Antenna not yet tuned"),
    ("5", "MCU Run, MSI 4 MHz, reader power-down", f"{T['run4']} mA", "DS11451 gives ~0.5 mA; peripheral clocks are on"),
    ("6", "MCU Sleep, MSI 4 MHz, SysTick every 1 ms", f"{T['sleep4']} mA", "The loop's mode in USB sessions"),
    ("7", "Green LED on (470 R from 3.3 V)", f"{T['led_g']} mA", ""),
    ("8", "Red LED on", f"{T['led_r']} mA", ""),
    ("9", "Vibration motor on (coin ERM on BAT+)", f"{T['motor']} mA", ""),
    ("10", "Standby (switched off)", f"{T['off_ua']} uA", "4.8 uA before the reader's SPI pins were pulled down in Standby"),
]
meas_table = table(["Mode", "State held", "Current", "Note"], meas_rows, align=["r", "l", "r", "l"], cls="assump")

assump_rows = [
    ("STM32L432 Run, 24 MHz / 80 MHz", f"{T['run24']} / {T['run80']} mA", "-", "DS11451 typ., Range 1 (not used while scanning)"),
    ("ADC on, extra over Run", f"{T['adc_extra']} mA", "-", "DS11451"),
    ("Flash program / erase", f"{T['flash_op']} mA", "-", "DS11451; 22 ms per page erase"),
    ("Button pull-ups (R8 10k + internal)", f"{T['button']} mA", "-", "Only while pressed"),
    ("Field time per read / per empty poll", f"{T['poll_ms']:.0f} / {T['empty_poll_ms']:.0f} ms", f"{P['poll_ms']:.0f} / {P['empty_poll_ms']:.0f} ms",
     "5 ms guard + 1 ms tick, then REQA..SELECT at 106 kbit/s"),
    ("Reader wake-up mode average", f"{T['nfc_wu_ua']:.0f} uA", f"{P['nfc_wu_ua']:.0f} uA", "Measured range: 117-204 uA on the meter"),
    ("False wake-ups while scanning", f"{T['false_per_min']:g} / min", f"{P['false_per_min']:g} / min", "Not measured yet; read dbg_nfc_false_wakes"),
    ("Cell self-discharge", f"{T['self_dis_pct']:g} %/month", f"{P['self_dis_pct']:g} %/month", "Typical Li-ion at room temperature"),
    ("Usable capacity of 1000 mAh", f"{usable_t:.0f} mAh", f"{usable_p:.0f} mAh", "Cut-off at 3.3 V, ageing margin"),
]
assump_table = table(["Quantity", "Typical", "Pessimistic", "Source / basis"], assump_rows,
                     align=["l", "r", "r", "l"], cls="assump")

clock_rows = [
    ("Scanning (SYSCLK/HCLK)", "MSI range 6, <b>4 MHz</b>", "Range 1, 0 wait states",
     "Reset clock and the Stop 2 wake-up clock (STOPWUCK = MSI): no PLL to relock on each wake-up. "
     "The work is time-bound (5 ms field guard, 106 kbit/s frames, ADC sampling), so a faster clock only raises the current for the same window."),
    ("USB session", "MSI range 9, <b>24 MHz</b>", "Range 1, 1 wait state",
     "USB FS needs HCLK above 14.2 MHz (RM0394). 24 MHz is the first MSI step with margin for rendering CSV sectors on demand. "
     "Powered from VBUS, so it costs the battery nothing."),
    ("USB kernel clock", "<b>HSI48</b> trimmed by CRS", "Locks to the host's 1 kHz SOF",
     "Meets the USB ±0.25 % tolerance with no crystal and no PLL. Stopped outside USB sessions."),
    ("RTC and Stop 2 timer", "<b>LSE 32.768 kHz</b>", "RTC; LPTIM1 at LSE / 8 (16-bit)",
     "Runs through Stop 2 for well under 1 uA. LPTIM1 counts 4096 Hz, so its 16-bit count wraps every 16 s and one Stop 2 sleep can last "
     "up to 15 s (the loop asks for at most 5 s); resolution 244 us. A wake from the wrap alone goes straight back to Stop 2."),
    ("Reader SPI", "4 MHz / 8 = <b>500 kHz</b>", "Mode 1, ST25R3916 max 10 MHz",
     "Well inside the chip's limit. Register traffic during a read adds an estimated 2-3 ms of field-on time; a /2 prescaler (2 MHz) would cut it to under 1 ms."),
    ("ADC", "HCLK / 4 = <b>1 MHz</b>", "640.5-cycle sample",
     "0.64 ms per channel gives the 1.7 MOhm divider time to settle; the ADC is powered only for each sample."),
    ("Reader crystal", "<b>27.12 MHz</b>", "2 x 13.56 MHz carrier",
     "Fixed by ISO 14443 (fc = 13.56 MHz). Runs only in Ready mode or for each wake-up measurement."),
]
clock_table = table(["Clock domain", "Selection", "Setting", "Why"], clock_rows, align=["l", "l", "l", "l"], cls="clock")

def wu_saving(period_ms):
    """Idle saving from a longer wake-up period: the measured average scales with the measurement rate."""
    return T["nfc_wu_ua"] * (1 - WU_PERIOD_MS / period_ms)


improve_rows = [
    ("Wake-up period 100 ms to 200 ms", f"-{wu_saving(200):.0f} uA idle",
     f"{wu_saving(200)/idle_t*100:.0f} % of idle; a card is noticed up to 100 ms later"),
    ("Wake-up period 100 ms to 400 ms", f"-{wu_saving(400):.0f} uA idle",
     f"{wu_saving(400)/idle_t*100:.0f} % of idle; up to 400 ms to notice a card"),
    ("Tune the antenna: field 203 mA to ~100 mA", f"-{(T['field']-100)*(T['poll_ms']+T['empty_poll_ms'])/1000:.1f} mC per tap",
     f"{(T['field']-100)*(T['poll_ms']+T['empty_poll_ms'])/ev_t['tap']*100:.0f} % of each tap; probably cheaper wake-up measurements too"),
    ("SPI prescaler /8 to /2", f"-{T['field']*1.4/1000:.2f} mC per read", f"{T['field']*1.4/ev_t['tap']*100:.0f} % of each tap"),
    ("Motor pulse 60 ms to 40 ms", f"-{T['motor']*20/1000:.1f} mC per tap", f"{T['motor']*20/ev_t['tap']*100:.0f} % of each tap"),
    ("Heartbeat off", f"-{T['led_g']*1000*HB_ON_MS/HB_PERIOD_MS:.1f} uA idle",
     f"{T['led_g']*1000*HB_ON_MS/HB_PERIOD_MS/idle_t*100:.0f} % of idle; no sign the unit is on"),
]
improve_table = table(["Change", "Saving", "Effect"], improve_rows, align=["l", "r", "l"])

done_table = table(["Change", "Before", "After"], [
    ("Debug clocks off in Stop 2 / Standby (DBGMCU_CR cleared at boot)", "0.3 mA switched off", "4.8 uA"),
    ("Reader SPI, LED and motor pins pulled down in Standby", "4.8 uA switched off", f"{T['off_ua']} uA"),
    ("Heartbeat flash", "30 ms every 4 s", "10 ms every 5 s"),
    ("Longest idle sleep (APP_SLEEP_MAX_MS); LPTIM1 / 8", "1 s (1.35 wakes/s)", "5 s (0.4 wakes/s)"),
    ("Battery sample placed on the heartbeat's wake", "own wake every 10 s", "none extra"),
    ("Empty polls before re-arming (APP_NFC_REMOVE_MISSES)", "3", "1"),
    ("Accepted-card buzz (APP_FB_ACCEPT_VIB_MS)", "90 ms", "60 ms"),
], align=["l", "r", "r"])

measured_table = table(["Quantity", "Value", "Note"], [
    ("Flash size (0x1FFF75E0)", "128 kB", "Fitted part is the STM32L432KB"),
    ("Cell voltage (dbg_battery_mv)", "4,152 mV", "Three samples 10 s apart: 4152, 4149, 4154 mV"),
    ("State of charge (dbg_battery_percent)", "96 %", "From the firmware's Li-ion voltage curve"),
    ("ADC counts at PA7", "1,876", "PA7 = 3308 mV x 1876 / 4095 = 1515 mV"),
    ("VDDA from VREFINT", "3,308 mV", "VREFINT_CAL 1652 / 1497 counts x 3000 mV"),
    ("Divider check", "4,153 mV", "1515 mV x (4.7 M + 2.7 M) / 2.7 M"),
], align=["l", "r", "l"])

COST = cm.compute()
R = cm.LKR_PER_USD


def usd(v):
    return f"{v:,.0f}" if v >= 1000 else f"{v:,.2f}"


cost_lines = (("parts", "Electronic parts (LCSC)"), ("pcb", "PCB fabrication"), ("asm", "PCB assembly (JLCPCB)"),
              ("offb", "Battery, motor, case, box"), ("freight", "Freight to Colombo"),
              ("tax", f"Import duty, VAT, levies ({cm.IMPORT_TAX*100:.0f} %)"),
              ("labour", "Final assembly and test (Sri Lanka)"), ("scrap", "Yield loss"))
cost_rows = [(lab, *[usd(c[k]) if c[k] > 0 else "-" for c in COST]) for k, lab in cost_lines]
cost_rows += [
    (("<b>Order total, USD</b>", *[f"<b>{usd(c['total'])}</b>" for c in COST]), "cls:total"),
    ("Order total, LKR", *[f"{c['total']*R:,.0f}" for c in COST]),
    (("<b>Cost per unit, USD</b>", *[f"<b>{c['unit']:,.2f}</b>" for c in COST]), "cls:total"),
    (("<b>Cost per unit, LKR</b>", *[f"<b>{c['unit']*R:,.0f}</b>" for c in COST]), "cls:"),
    (f"Price per unit at {cm.MARGIN*100:.0f} % margin, USD", *[f"{c['unit']/(1-cm.MARGIN):,.2f}" for c in COST]),
    (f"Price per unit at {cm.MARGIN*100:.0f} % margin, LKR", *[f"{c['unit']/(1-cm.MARGIN)*R:,.0f}" for c in COST]),
]
cost_table = table(["USD unless stated", "1 unit", "100 units", "1 000 units", "10 000 units"], cost_rows, cls="keep")

breaks_rows = []
for lcsc, (name, b, est) in cm.BREAKS.items():
    breaks_rows.append((name, *[f"{b[n]:.3f}" + ("*" if n in est else "") for n in cm.SCALES]))
breaks_rows.append((("<b>All electronic parts, per board</b>", *[f"<b>{c['parts_per_board']:.2f}</b>" for c in COST]), "cls:total"))
breaks_table = table(["Part (USD each)", "1", "100", "1 000", "10 000"], breaks_rows, cls="keep")

cost_assump_rows = [
    ("Exchange rate", f"1 USD = {R:.0f} LKR", f"Rate on {cm.LKR_RATE_ON}; it moved 328-332 over the month before"),
    ("Electronic parts", f"LCSC, {cm.PRICES_ON}", "Price breaks above; other parts at the BOM's minimum-lot price x "
     + ", ".join(f"{cm.OTHER_FACTOR[n]:g}" for n in cm.SCALES)),
    ("PCB", "$2 for 5; then $" + " / $".join(f"{cm.PCB_EACH[n]:.2f}" for n in cm.SCALES[1:]), "2 layers, 60 x 89 mm, per board (estimate)"),
    ("Assembly", f"JLCPCB Economic to 100, Standard above", f"{cm.SMT_JOINTS} SMT + {cm.THT_JOINTS} hand-soldered joints, "
     f"{cm.EXT_TYPES} Extended part types at $3.07 each per order"),
    ("Battery", " / ".join(f"${cm.BATTERY[n]:.2f}" for n in cm.SCALES), "1000 mAh LiPo with protection board and JST XA lead (estimate)"),
    ("Motor", " / ".join(f"${cm.MOTOR[n]:.2f}" for n in cm.SCALES), "10 mm coin ERM with leads (estimate)"),
    ("Case", " / ".join(f"${cm.ENCL[n]:.2f}" for n in cm.SCALES), "3D printed to 100; aluminium tool ($3 000) at 1 000; steel tool ($6 000) at 10 000"),
    ("Freight", " / ".join(f"${cm.FREIGHT[n]:,.0f}" for n in cm.SCALES), "Whole order, China to Colombo; lithium cells ship as dangerous goods"),
    ("Import charges", f"{cm.IMPORT_TAX*100:.0f} % of CIF", "Duty (HS code dependent) + 18 % VAT + 2.5 % SSCL + levies; VAT is reclaimable when registered"),
    ("Labour", f"${cm.LABOUR_USD_H:.2f}/h; " + " / ".join(f"{cm.LABOUR_MIN[n]} min" for n in cm.SCALES),
     "Fit battery, motor and case, flash, test; plus a $300-600 test jig from 1 000"),
    ("Yield", " / ".join(f"{cm.YIELD[n]*100:.0f} %" for n in cm.SCALES), "Units that pass test"),
]
cost_assump_table = table(["Item", "1 / 100 / 1 000 / 10 000", "Basis"], cost_assump_rows, align=["l", "l", "l"], cls="assump")
cost_items = [(f"{c['n']:,} unit" + ("s" if c['n'] > 1 else ""), c["unit"]) for c in COST]

# --------------------------------------------------------------------------
# Page
# --------------------------------------------------------------------------
css = """
@page { size: A4; margin: 16mm 16mm 18mm 16mm;
  @bottom-left { content: "RFID Attendance Logger - Power, capacity and cost"; font: 8pt Inter, sans-serif; color: #8a8984; }
  @bottom-right { content: "Page " counter(page) " of " counter(pages); font: 8pt Inter, sans-serif; color: #8a8984; } }
@page :first { @bottom-left { content: none; } }
:root { --ink:#0b0b0b; --ink2:#52514e; --muted:#8a8984; --rule:#e4e3dd; --soft:#f4f3ef; --blue:#2a78d6; --orange:#eb6834; }
* { box-sizing: border-box; }
html, body { background: #ffffff; }
body { font-family: Inter, "Noto Sans", sans-serif; color: var(--ink); font-size: 9.6pt; line-height: 1.5; margin: 0;
  -webkit-print-color-adjust: exact; print-color-adjust: exact; font-feature-settings: "tnum" 1, "cv11" 1; }
h1 { font-family: "Inter Display", Inter, sans-serif; font-size: 27pt; line-height: 1.1; margin: 0 0 6pt; font-weight: 700; letter-spacing: -0.5pt; }
h2 { font-family: "Inter Display", Inter, sans-serif; font-size: 15pt; margin: 22pt 0 6pt; font-weight: 650; letter-spacing: -0.2pt;
  padding-top: 8pt; border-top: 1.5pt solid var(--ink); break-after: avoid; }
h2 .num { color: var(--blue); margin-right: 6pt; }
h3 { font-size: 10.5pt; margin: 14pt 0 4pt; font-weight: 650; break-after: avoid; }
p { margin: 0 0 7pt; }
.lede { font-size: 11.5pt; color: var(--ink2); max-width: 155mm; margin-bottom: 14pt; }
.cover { padding: 6mm 0 2mm; }
.eyebrow { font-size: 8.5pt; text-transform: uppercase; letter-spacing: 1.2pt; color: var(--blue); font-weight: 650; margin-bottom: 8pt; }
.meta { display: flex; gap: 22pt; font-size: 8.5pt; color: var(--ink2); margin: 10pt 0 18pt; }
.meta b { color: var(--ink); font-weight: 600; display: block; }
.tiles { display: grid; grid-template-columns: repeat(4, 1fr); gap: 8pt; margin: 6pt 0 14pt; }
.tile { background: var(--soft); border-radius: 6pt; padding: 10pt 11pt 9pt; }
.tile .k { font-size: 7.6pt; text-transform: uppercase; letter-spacing: 0.7pt; color: var(--ink2); font-weight: 600; }
.tile .v { font-family: "Inter Display", Inter, sans-serif; font-size: 21pt; font-weight: 700; line-height: 1.15; margin-top: 3pt; letter-spacing: -0.4pt; }
.tile .v small { font-size: 10pt; font-weight: 600; color: var(--ink2); margin-left: 2pt; letter-spacing: 0; }
.tile .s { font-size: 7.8pt; color: var(--ink2); margin-top: 2pt; line-height: 1.35; }
.tile.accent { background: #e9f1fb; }
.tile.accent .v { color: #1d5fae; }
table { width: 100%; border-collapse: collapse; margin: 6pt 0 10pt; font-size: 8.6pt; break-inside: auto; }
thead { display: table-header-group; }
tr { break-inside: avoid; }
th { text-align: left; font-weight: 600; color: var(--ink2); font-size: 8pt;
  border-bottom: 1pt solid var(--ink); padding: 4pt 6pt; vertical-align: bottom; }
td { border-bottom: 0.6pt solid var(--rule); padding: 4pt 6pt; vertical-align: top; }
.r { text-align: right; white-space: nowrap; }
.l { text-align: left; }
tr.total td { border-top: 1pt solid var(--ink); border-bottom: none; font-weight: 600; }
table.clock td:first-child { font-weight: 600; width: 22%; }
table.clock td:nth-child(2) { width: 18%; }
table.clock td:nth-child(3) { width: 18%; color: var(--ink2); }
table.events td:nth-child(2) { color: var(--ink2); }
table.assump td:last-child { color: var(--ink2); }
table.scen td:first-child, table.scen th:first-child { width: 34%; }
table.scen, table.keep { break-inside: avoid; }
.figure { margin: 8pt 0 12pt; break-inside: avoid; }
.figure .cap { font-size: 8.2pt; color: var(--ink2); margin-top: 3pt; }
.figure .title { font-weight: 650; font-size: 9.4pt; margin-bottom: 4pt; }
.chart { width: 100%; height: auto; display: block; }
.chart .grid { stroke: #ecebe6; stroke-width: 1; }
.chart .axis { stroke: #8a8984; stroke-width: 1; }
.chart .tick { font: 8px Inter, sans-serif; fill: #6b6a66; }
.chart .lab { font: 9.5px Inter, sans-serif; fill: #0b0b0b; }
.chart .val { font: 600 9.5px Inter, sans-serif; fill: #0b0b0b; font-feature-settings: "tnum" 1; }
.chart .seg { font: 600 8.5px Inter, sans-serif; }
.calc { background: var(--soft); border-left: 2.5pt solid var(--blue); padding: 7pt 10pt; margin: 6pt 0 10pt; font-size: 8.8pt;
  break-inside: avoid; border-radius: 0 4pt 4pt 0; }
code { font-family: "DejaVu Sans Mono", monospace; font-size: 8.2pt; }
.calc .eq { font-family: "DejaVu Sans Mono", monospace; font-size: 8.3pt; line-height: 1.6; white-space: pre-wrap; }
.note { font-size: 8.4pt; color: var(--ink2); }
.callout { border: 1pt solid var(--rule); border-radius: 6pt; padding: 8pt 11pt; margin: 8pt 0 12pt; break-inside: avoid; }
.two { display: grid; grid-template-columns: 1fr 1fr; gap: 14pt; }
.pb { break-before: page; }
.keep { break-inside: avoid; }
ul { margin: 2pt 0 8pt 14pt; padding: 0; }
li { margin-bottom: 3pt; }
.legend { display: flex; flex-wrap: wrap; row-gap: 3pt; gap: 3pt 14pt; font-size: 8pt; color: var(--ink2); margin-top: 2pt; }
.legend span { white-space: nowrap; }
.legend span::before { content: ""; display: inline-block; width: 9pt; height: 9pt; border-radius: 2pt; margin-right: 4pt; vertical-align: -1pt; background: var(--c); }
.measured { display: inline-block; background: #e8f5ee; color: #0f6b47; font-weight: 600; font-size: 7.6pt; padding: 1pt 5pt; border-radius: 3pt;
  text-transform: uppercase; letter-spacing: 0.5pt; }
"""

fill_lo, fill_hi = min(fills), max(fills)
html = f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Power and Cost Report</title><style>{css}</style></head><body>

<section class="cover">
  <div class="eyebrow">Bench measurement and model &middot; Firmware and hardware rev 0.1</div>
  <h1>RFID Attendance Logger<br>Power, Capacity and Cost</h1>
  <p class="lede">Current draw for each operating mode and event, when the MCU and the reader are awake,
  the reasoning behind each clock frequency, the expected number of card reads and the battery life on a 1000 mAh, 3.7 V Li-ion cell,
  how the STM32L432KB's 128 kB of flash is shared between the program and the attendance log,
  and what a finished unit costs to make at 1 to 10 000 units, in US dollars and Sri Lankan rupees.</p>
  <div class="meta">
    <div><b>Hardware</b>STM32L432KB (128 kB) + ST25R3916</div>
    <div><b>Battery</b>1000 mAh, 3.7 V nominal (3.7 Wh)</div>
    <div><b>Date</b>{today}</div>
    <div><b>Status</b>Currents measured {MEASURED_ON}</div>
  </div>
  <div class="tiles">
    <div class="tile accent"><div class="k">Battery life</div><div class="v">{life4_t:.0f}<small>months</small></div>
      <div class="s">4 lectures/day, 100 students, incl. self-discharge ({life4_p:.0f} pessimistic)</div></div>
    <div class="tile"><div class="k">Idle scanning</div><div class="v">{idle_t:.0f}<small>uA</small></div>
      <div class="s">Reader wake-up mode is {T['nfc_wu_ua']/idle_t*100:.0f} % of it</div></div>
    <div class="tile"><div class="k">Switched off</div><div class="v">{off_t:.1f}<small>uA</small></div>
      <div class="s">Standby, measured; {usable_t/(off_t/1000)/24/365:.0f} years on the device alone</div></div>
    <div class="tile"><div class="k">Per card tap</div><div class="v">{uah(ev_t['tap']):.1f}<small>uAh</small></div>
      <div class="s">{ev_t['tap']/1000:.1f} mC; {LOG_CAP:,} records fit in flash</div></div>
  </div>
</section>

<h2><span class="num">1</span>Summary</h2>
<p>The logger spends nearly all of its life in one of two low-power states. Switched off, it sits in Standby and draws
<b>{off_t:.1f} uA</b> (measured). Switched on and waiting for cards, the MCU sleeps in Stop 2 and wakes only for a 10 ms heartbeat flash
every 5 s, while the ST25R3916 checks the antenna itself every 100 ms. That idle state draws about <b>{idle_t:.0f} uA</b>, and
{T['nfc_wu_ua']/idle_t*100:.0f} % of it is the reader's wake-up mode. The large currents ({T['field']:.0f} mA for the RF field, {T['motor']:.1f} mA for
the motor) flow only for milliseconds per tap, so a whole 100-student lecture costs about <b>{lect_t/1000:.2f} mAh</b> ({lect_t/1000*BATT_V:.2f} mWh).</p>
<p>At four lectures a day, the device uses about <b>{dev_m:.0f} mAh a month</b>. With the cell's self-discharge (about {sd_m:.0f} mAh a month)
that gives roughly <b>{life4_t:.0f} months per charge</b>, or {life4_t_nosd:.0f} months from the device's draw alone.</p>
<p>The program and the log share the STM32L432KB's 128 kB flash: 72 kB for the program ({IMG_OG/1024:.1f} kB at -Og) and 56 kB for the log,
<b>{LOG_CAP:,} records</b> or about {LECT100} lectures of 100 students. The log fills long before the battery empties
(in about {LECT100/4:.0f} teaching days at four lectures a day), so the PC sync, which clears the log, sets the routine, not the charger.</p>
<p>A finished unit costs about <b>US${COST[2]['unit']:.0f} (LKR {COST[2]['unit']*cm.LKR_PER_USD:,.0f})</b> to make at 1 000 units and
US${COST[3]['unit']:.0f} (LKR {COST[3]['unit']*cm.LKR_PER_USD:,.0f}) at 10 000, landed in Sri Lanka and tested (section 11).</p>
<div class="callout"><b>Confidence.</b> Every current in this report except the 24/80 MHz run currents, the ADC and the flash was measured on the board
({MEASURED_ON}) with a multimeter in series with the cell, using a power-test build that holds each load steady (section 3).
Event charges are those currents multiplied by the firmware's timings, which come from its constants, not from a scope.
The reader's wake-up mode average is the least certain figure: the meter read 117-204 uA while it averaged the reader's 100 ms measurements,
so the pessimistic column uses 204 uA and 4 false wake-ups a minute. The RF field current is for the untuned antenna.</div>

<h2><span class="num">2</span>Clock frequency selection</h2>
<p>Each clock was picked for the job it does. The scanning clock matters most, because it runs on every wake-up.</p>
{clock_table}

<h3>Why 4 MHz and not faster</h3>
<p>The MCU's awake time is set by the radio and the protocol rather than by computation: a read waits out a 5 ms field guard,
exchanges ISO 14443 frames at 106 kbit/s, and busy-waits on the reader's IRQ line. For a fixed window <i>t</i>, the charge is
<i>Q = I<sub>run</sub>(f) &times; t</i>, so the lowest clock that meets the deadlines wins.</p>
<div class="calc"><div class="eq">MCU charge during one 15 ms read window (4 MHz measured, 24/80 MHz datasheet):
{eqs([
  ("4 MHz", f"{T['run4']:.2f} mA x 15 ms", f"{T['run4']*15:.1f} uC"),
  ("24 MHz", f"{T['run24']:.1f} mA x 15 ms", f"{T['run24']*15:.1f} uC"),
  ("80 MHz", f"{T['run80']:.1f} mA x 15 ms", f"{T['run80']*15:.1f} uC  + PLL lock per wake-up"),
  ("RF field", f"{T['field']:.0f} mA x {T['poll_ms']:.0f} ms", f"{T['field']*T['poll_ms']:.0f} uC"),
])}</div></div>
<div class="figure"><div class="title">Charge for one card read: MCU clock options against the RF field</div>
{svg_hbar(clock_items, "#2a78d6", "uC", fmt=lambda v: f"{v:,.1f} uC", ref="RF field for the same poll")}
<div class="cap">At 4 MHz the MCU is {T['run4']*15/(T['field']*T['poll_ms'])*100:.1f} % of a read's charge. At 80 MHz it would be about
{T['run80']*15/(T['field']*T['poll_ms'])*100:.0f} %, for no gain in speed.</div></div>
<p>For purely computational work (one pass of the main loop is roughly 2000 cycles), current per MHz is close to constant,
so finishing faster and sleeping longer gains little: 0.5 ms at {T['run4']:.1f} mA ({T['run4']*0.5:.2f} uC) at 4 MHz against 25 us at 9 mA (0.23 uC) at 80 MHz.
That small gain is lost to the PLL's start-up time and to the 4 flash wait states 80 MHz needs.</p>
<h3>Why not slower</h3>
<ul>
<li><b>SPI with the field on.</b> The SPI clock is SYSCLK / 8. At 2 MHz the reader's register traffic would take twice as long
while the {T['field']:.0f} mA field is on, costing more than the MCU saves.</li>
<li><b>ADC sampling.</b> The ADC clock is HCLK / 4. A slower clock stretches the 640.5-cycle sample and keeps the ADC powered longer.</li>
<li><b>Low-power run (at most 2 MHz) would barely help.</b> The MCU is awake about {WAKES_PER_S*WAKE_MS/10:.2f} % of the time in idle, so its run current
adds only ~{WAKES_PER_S*WAKE_MS/1000*T['run4']*1000:.1f} uA to the {idle_t:.0f} uA idle figure.</li>
</ul>
<h3>Voltage scaling</h3>
<p>The firmware keeps Range 1 at all clocks. Range 2 would cut run current by about 10 %, but that saves well under 0.1 uA in idle, while USB
needs Range 1 anyway. Staying in Range 1 avoids switching the regulator on every USB attach. Stop 2 and Standby use the low-power
regulator either way.</p>

<h2 class="pb"><span class="num">3</span>Measurements and assumptions</h2>
<p>Battery-side currents, measured on {MEASURED_ON} with a multimeter in series with the cell. The power-test build
(<code>make power-test</code>, <code>Bsp/Src/bsp_power_test.c</code>) runs no application: each press of the button holds the next load
steady, so the meter reads it instead of a millisecond pulse. The 3.3 V rail comes from a linear LDO, so the battery supplies the same current
the MCU and LEDs draw; the reader's VDD/VDD_TX and the motor sit straight on BAT+.</p>
<h3>Measured <span class="measured">Measured {MEASURED_ON}</span></h3>
{meas_table}
<h3>Changes made after the first measurements</h3>
{done_table}
<h3>Still estimated</h3>
{assump_table}
<h3>Measured on the board <span class="measured">Measured 07 October 2026</span></h3>
{measured_table}

<h2><span class="num">4</span>Operating modes</h2>
{table(["Mode", "Typical", "Pessimistic", "What is running"], mode_rows, align=["l", "r", "r", "l"])}
<div class="figure"><div class="title">Idle scanning current, {idle_t:.0f} uA in total</div>
{svg_hbar(idle_items, "#2a78d6", "uA", fmt=lambda v: f"{v:.2f} uA")}
<div class="cap">The reader's own antenna measurements are {T['nfc_wu_ua']/idle_t*100:.0f} % of idle current. The MCU, including its wake-ups,
is under {(T['stop2_base_ua'] + WAKES_PER_S*WAKE_MS*T['run4'])/idle_t*100+1:.0f} %.</div></div>
<div class="calc"><div class="eq">{eqs([
  ("Wake-up mode", f"mode 2 - mode 1 = {T['stop2_base_ua']+T['nfc_wu_ua']:.0f} - {T['stop2_base_ua']} uA", f"{T['nfc_wu_ua']:.1f} uA"),
  ("Heartbeat", f"{T['led_g']} mA x {HB_ON_MS} ms / {HB_PERIOD_MS} ms", f"{T['led_g']*1000*HB_ON_MS/HB_PERIOD_MS:.1f} uA"),
  ("Wake-ups", f"0.4 /s x 0.5 ms x {T['run4']} mA", f"{WAKES_PER_S*WAKE_MS/1000*T['run4']*1000:.2f} uA"),
  ("Sampling", f"1.5 ms x ({T['run4']} + {T['adc_extra']}) mA / 10 s", f"{BATT_SAMPLE_MS/1000*(T['run4']+T['adc_extra'])*1000/10:.2f} uA"),
  ("Off", "measured, mode 10", f"{off_t:.2f} uA"),
  ("Polling mode", f"{T['nfc_ready']} mA Ready + {T['field']:.0f} mA x 6 ms / 100 ms + MCU", f"{poll_mode_ma:.1f} mA"),
])}</div></div>
<p>The reader's wake-up mode is still far cheaper than polling: switching it off would take idle current from {idle_t:.0f} uA to about
{poll_mode_ma:.0f} mA, and battery life left switched on from {idle_life_h/24:.0f} days to {poll_life_h/24:.1f} days. At {WU_PERIOD_MS} ms it costs about
{T['nfc_wu_ua']*WU_PERIOD_MS/1000:.0f} uC per measurement, far more than the ~1 uC first assumed: each measurement starts the 27.12 MHz oscillator
and drives the untuned antenna. A longer period (section 10) or a tuned antenna is the largest remaining saving.</p>

<h2 class="pb"><span class="num">5</span>When the MCU and the reader are awake</h2>
<p>The two chips sleep independently. The MCU sleeps in Stop 2 and wakes on its own timer (LPTIM1) or on an interrupt: the button,
VBUS, or the reader's IRQ line. The ST25R3916 runs its own wake-up timer and wakes the MCU only when the antenna changes. Timings are the
firmware's; the currents beside each lane are the measured ones.</p>
<div class="figure"><div class="title">Idle scanning: ten seconds</div>
{svg_idle_timeline(T)}
<div class="legend"><span style="--c:{C_STOP2}">MCU Stop 2</span><span style="--c:{C_RUN}">MCU awake (~0.5 ms)</span>
<span style="--c:{C_WU}">Reader wake-up mode</span><span style="--c:{C_MEAS}">Reader antenna measurement</span><span style="--c:{C_LED}">LED on (10 ms)</span></div>
<div class="cap">The MCU wakes twice every 5 s: when the heartbeat LED goes on and 10 ms later when it goes off. The battery is sampled on the
first of those wakes every 10 s, so it adds no wake of its own. Meanwhile the reader measures the antenna 10 times a second, without the MCU.
Nothing else is due, so the loop sleeps the full 5 s (<code>APP_SLEEP_MAX_MS</code>).</div></div>
<div class="figure"><div class="title">The reader's wake-up measurements, every {WU_PERIOD_MS} ms</div>
{svg_wakeup_pulses(T)}
<div class="cap">Log scale. Ten times a second the ST25R3916 starts its oscillator, drives the antenna for a moment and compares the
amplitude with its reference; the MCU sleeps through all of it. When a card changes the amplitude past the window (the fourth
measurement), the reader raises its IRQ and the MCU wakes to read the card. The charge per measurement, {wu_pulse(T)[0]:.1f} uC, follows from the
measured {T['nfc_wu_ua']:.0f} uA average; how it divides between oscillator start-up and the field burst is an illustration
(assumed {WU_OSC_MS:g} ms start-up) until the pulse is captured with a scope across a shunt resistor.</div></div>
<div class="figure"><div class="title">One accepted card tap</div>
{svg_tap_timeline(T)}
<div class="legend"><span style="--c:{C_STOP2}">MCU Stop 2</span><span style="--c:{C_RUN}">MCU awake</span>
<span style="--c:{C_WU}">Wake-up mode</span><span style="--c:{C_READY}">Ready, {T['nfc_ready']:.2f} mA</span>
<span style="--c:{C_FIELD}">RF field on, {T['field']:.0f} mA</span><span style="--c:{C_MOTOR}">Motor</span><span style="--c:{C_LED}">LED</span></div>
<div class="cap">The card is noticed at the reader's next measurement, at most {WU_PERIOD_MS} ms after it arrives. The IRQ wakes the MCU, which turns the
field on and sleeps through the 6 ms guard in Stop 2, then runs REQA, anticollision and SELECT (~4 ms), logs the card and starts the pattern.
The reader stays in Ready mode while the pattern plays, so the motor never runs with the field on. At the end of the pattern one empty poll
(the card has gone) re-arms wake-up mode. Five seconds after the last tap the MCU wakes once more to write the RAM buffer to flash.</div></div>
<div class="figure"><div class="title">A false wake-up</div>
{svg_false_timeline(T)}
<div class="cap">A measurement strays past the window with no card there (noise, a hand, drift). One empty poll, about {T['empty_poll_ms']:.0f} ms of field,
and the reader is armed again with a fresh reference: {ev_t['false_wake']/1000:.1f} mC each, against 5.5 mC with the three empty polls used before.</div></div>

<h2><span class="num">6</span>Charge per event</h2>
<p>Charge = current x time for each load, summed over the event. 1 uAh = 3.6 mC.</p>
{event_table}
<div class="figure"><div class="title">One accepted card tap, {ev_t['tap']/1000:.1f} mC ({uah(ev_t['tap']):.2f} uAh)</div>
{svg_hbar(tap_items, "#2a78d6", "mC", fmt=lambda v: qfmt(v*1000))}
<div class="cap">The field (read plus one empty poll) and the motor are about {(T['field']*(T['poll_ms']+T['empty_poll_ms']) + T['motor']*FB_ACCEPT_VIB)/ev_t['tap']*100:.0f} % of a tap.
Keeping the reader in Ready mode during the pattern is most of the rest.</div></div>
<div class="calc"><div class="eq">{eqs([
  ("Motor", f"{T['motor']:.1f} mA x {FB_ACCEPT_VIB} ms", f"{T['motor']*FB_ACCEPT_VIB/1000:.2f} mC"),
  ("Ready mode", f"{T['nfc_ready']} mA x {READY_AFTER_TAP_MS} ms (pattern)", f"{T['nfc_ready']*READY_AFTER_TAP_MS/1000:.2f} mC"),
  ("Empty poll", f"{EMPTY_POLLS} x {T['field']:.0f} mA x {T['empty_poll_ms']:.0f} ms", f"{EMPTY_POLLS*T['field']*T['empty_poll_ms']/1000:.2f} mC"),
  ("Read", f"{T['field']:.0f} mA x {T['poll_ms']:.0f} ms (guard, REQA, anticollision, SELECT)", f"{T['field']*T['poll_ms']/1000:.2f} mC"),
  ("LED", f"{T['led_g']} mA x 250 ms", f"{T['led_g']*250/1000:.2f} mC"),
  ("Total", "incl. re-arm, MCU, oscillator, flash", f"{ev_t['tap']/1000:.2f} mC = {uah(ev_t['tap']):.2f} uAh"),
])}</div></div>

<h2><span class="num">7</span>A lecture, and battery life</h2>
<p>The model lecture: the lecturer powers the unit on and starts a lecture with a button hold; 100 students tap over 15 minutes,
5 of them twice; 3 minutes after the last tap the unit switches itself off. It is on for 18 minutes.</p>
<div class="two">
<div>{table(["Part", "uAh", "Share"], lect_rows)}</div>
<div class="calc" style="align-self:start"><div class="eq">Per month at 4 lectures/weekday:
  lectures  = 4 x 21.7          = {4*WEEKDAYS_PER_MONTH:.1f}
  device    = {4*WEEKDAYS_PER_MONTH:.1f} x {lect_t/1000:.3f} mAh
            + {off_t:.2f} uA x 730 h    = {dev_m:.1f} mAh
  self-dis. = 2 % x 1000 mAh    = {sd_m:.1f} mAh
  total                         = {tot_m:.1f} mAh

Life = {usable_t:.0f} mAh / {tot_m:.1f} mAh = {life4_t:.1f} months
     (device only: {life4_t_nosd:.1f} months)</div></div>
</div>
<div class="figure"><div class="title">Months per charge against lectures per day</div>
{svg_life_chart()}
<div class="cap">Light use is limited by the cell's self-discharge, not the device. At 4 lectures a day a charge lasts about {life4_t:.0f} months,
with idle scanning and false wake-ups a large share of each lecture.</div></div>
{scen_table}
<p class="note">Shelf time when never switched on: {usable_t:.0f} mAh / {off_t:.1f} uA = {usable_t/(off_t/1000)/24/365:.0f} years from the device's draw alone,
but self-discharge empties the cell in roughly {usable_t/sd_m/12:.1f} years. Store the device charged and recharge it each term.
All values are at room temperature; Li-ion capacity falls 10-20 % near 0 °C and with ageing, partly covered by the usable-capacity factor.</p>

<h2><span class="num">8</span>Card reads per charge</h2>
<p>A tap alone costs {uah(ev_t['tap']):.2f} uAh, so a charge could fund <b>{taps_bound:,.0f}</b> taps if nothing else drew current.
With each lecture's idle time, power-on and power-off, duplicates and false wake-ups shared out among its students,
the effective cost per student is higher, especially in small classes:</p>
{reads_table}
<p>Even small classes get over {min(usable_t*1000/(lecture(T, n)[1]/n) for n in (30, 60, 100, 200))/1000:.0f} 000 reads per charge (self-discharge aside), which is {fill_lo:.0f} to {fill_hi:.0f} times the
{LOG_CAP:,} records the flash can hold. Before the battery is a limit, the log has to be read and cleared by the PC app many times over
(by <code>#CLEARLOG</code> when it starts a lecture).</p>

<h2><span class="num">9</span>Flash: program and attendance log</h2>
<p>The fitted MCU is the <b>STM32L432KB</b>: its flash-size word at <code>0x1FFF75E0</code> reads 128 (measured on the board).
Its 128 kB is 64 pages of 2 kB, erased a page at a time, and the program and the log must share them. Earlier firmware assumed the
256 kB STM32L432KC and kept its data at <code>0x08020000</code>, which the KB does not specify; the layout below fits the KB.</p>
<div class="figure"><div class="title">Flash map, one block per 2 kB page</div>
{svg_flash_map()}
<div class="legend"><span style="--c:#2a78d6">Program (-Og image)</span><span style="--c:#e3edf9">Free code space</span>
<span style="--c:#eda100">Config (device ID)</span><span style="--c:#1baf7a">Attendance log</span></div></div>

<h3>Program size</h3>
<p>Measured with the project's own toolchain (arm-none-eabi-gcc 13.3, nano libc, unused sections removed):</p>
{image_table}
<div class="figure"><div class="title">Where the {IMG_OS/1024:.1f} kB -Os image goes</div>
{svg_hbar(OS_BREAKDOWN, "#2a78d6", "bytes", fmt=lambda v: f"{v/1024:.1f} kB")}
<div class="cap">Application code and ST's HAL are about three quarters of the image. The HAL is used through its generic drivers;
replacing it with direct register code could save several kB more, at the cost of portability.</div></div>

<div class="keep"><h3>Choosing the split</h3>
<p>Every page given to code is 254 records fewer in the log. The CubeIDE Debug build used to compile at -O0 ({IMG_O0/1024:.0f} kB), which fits no
useful split, so it now builds at -Og like the Makefile. With -Og as the largest build that must fit:</p>
{split_table}</div>
<p>72 kB of code leaves {(CODE_BYTES-IMG_OG)/1024:.1f} kB ({100*(CODE_BYTES-IMG_OG)/CODE_BYTES:.0f} %) for future features at -Og and still gives the log
{LOG_CAP:,} records. A 64 kB code region would add {(64-32-1-LOG_PAGES)*RECS_PER_PAGE:,} records (+{100*((64-32-1)/LOG_PAGES-1):.0f} %) but leave only
{(65536-IMG_OG)/1024:.1f} kB at -Og, forcing -Os for debugging. If records ever matter more than debugging, -Os with link-time optimisation
({IMG_OS_LTO/1024:.1f} kB) would make the 64 kB split comfortable.</p>
<p>The linker script (<code>STM32L432KBUX_FLASH.ld</code>) gives the program 72 kB, so an image that outgrows it fails to link instead of
overwriting the log. Link-time and compile-time checks keep the linker script, the board header and the log layout in step.</p>

<h3>Log capacity</h3>
<div class="calc"><div class="eq">Page       = 2048 bytes = 256 double-words
Records    = 256 - 1 header - 1 footer          = 254 per page
Log        = {LOG_PAGES} pages x 254                     = {LOG_CAP:,} records (8 bytes each: card ID + time)
Lecture    = 1 header + ceil((module + lecture + 2 NULs) / 4) text records
           = 1 + ceil((6 + 10 + 2) / 4) = 6 for "EE4020" / "Lecture 12" (range 4-16)
Lectures   = floor({LOG_CAP:,} / (N students + 6))</div></div>
<div class="figure"><div class="title">Lectures the log can hold before a PC sync</div>
{svg_flash_chart(flash_sizes)}
<div class="cap">Duplicates are not stored, so N counts distinct students in each lecture.</div></div>
{flash_table}
<ul>
<li><b>Students per lecture:</b> up to <b>{LOG_CAP-MARKER_RECS:,}</b> in a single lecture after a clear (log size minus one marker).</li>
<li><b>Registered students:</b> not limited by the device. It keeps no card list; any 32-bit card ID below 0xFFFFFF00 is stored,
and the PC app decides who each card belongs to.</li>
<li><b>When the log is full</b> new taps are refused with the red error pattern, because <code>APP_LOG_WRAP_WHEN_FULL</code> is 0. Up to
128 records wait in RAM (flushed at 80 % or 5 s after the last tap) and are written before power-off.</li>
<li><b>USB view:</b> a full log is {LOG_CAP:,} rows x 32 bytes = {LOG_CAP*32/1000:.0f} kB of <code>ATTEND.CSV</code>
({math.ceil(LOG_CAP*32/512)} clusters), well within FAT12's 4084-cluster limit.</li>
<li><b>Flash wear:</b> each log page is erased once per fill-and-clear cycle. At the STM32L4's {ENDURANCE:,}-cycle endurance, that is
{ENDURANCE:,} full logs (about {ENDURANCE*LOG_CAP/1e6:.0f} million records): not a limit in practice.</li>
<li><b>Moving a unit to this layout:</b> the new firmware does not read the old data at <code>0x08020000</code>. Import the unit's log
with the PC app before reflashing, and set its device ID again afterwards.</li>
</ul>

<div class="keep"><h2><span class="num">10</span>Where to save more power</h2>
<p>Ranked by effect. The reader's wake-up mode dominates idle current, so its period and the antenna tuning matter most;
the rest are small.</p>
{improve_table}</div>

<h2 class="pb"><span class="num">11</span>Manufacturing cost</h2>
<p>What a finished unit costs to make and land in Sri Lanka: the assembled board, the 1000 mAh cell, the motor, the case and box,
freight from China, import charges, and final assembly and test in Sri Lanka. Prices are in US dollars with rupees at
1 USD = {R:.0f} LKR. The model is <code>Firmware/docs/cost_model.py</code>; it reads the LCSC BOM workbook in <code>PCB/bom</code>.</p>
{cost_table}
<div class="figure"><div class="title">Cost per unit against order size</div>
{svg_hbar(cost_items, "#2a78d6", "USD", fmt=lambda v: f"${v:,.2f}  (LKR {v*R:,.0f})")}
<div class="cap">A single unit carries a whole minimum order: 5 PCBs, 2 assembled boards, JLCPCB's setup and
{cm.EXT_TYPES} Extended-part fees (${cm.EXT_TYPES*3.07:.0f}), and a courier. From 100 units the parts dominate, led by the STM32 and the ST25R3916.</div></div>

<div class="two">
<div><h3>Where the money goes</h3>
<ul>
<li><b>1 unit:</b> assembly fees (${COST[0]['asm']:.0f} for the order) and parts in minimum lots (${COST[0]['parts_per_board']:.0f} a board).
Hand-soldering the prototype saves the assembly fees.</li>
<li><b>100 units:</b> parts are ${COST[1]['parts_per_board']:.2f} a board; the two ICs are over half.</li>
<li><b>1 000 units:</b> the case tooling adds $3.00 a unit; parts fall to ${COST[2]['parts_per_board']:.2f}.</li>
<li><b>10 000 units:</b> parts ${COST[3]['parts_per_board']:.2f}; import charges are the second largest line.</li>
<li><b>Selling price</b> = cost / (1 - margin); the table shows a {cm.MARGIN*100:.0f} % gross margin as an example.</li>
</ul></div>
<div><h3>Key part prices</h3>
{breaks_table}
<p class="note">* extrapolated (LCSC lists no break at that quantity).</p></div>
</div>

<h3>Assumptions</h3>
{cost_assump_table}
<div class="callout"><b>Risks and exclusions.</b> LCSC held only 27 ST25R3916 on {cm.PRICES_ON} and Digi-Key none, so 100 units or more
need a quote from ST or a distributor. The duty rate depends on the HS code the goods are cleared under. Not included: the optional PN532
module, a USB cable, radio type approval (TRCSL, for a 13.56 MHz transmitter), firmware and engineering time, warranty and distribution.
R6 is still 1 kΩ (1 A charge current); the BOM's 2 kΩ alternate (500 mA) suits a USB-C sink and costs the same.</div>
<p class="note">Sources: lcsc.com product pages C2928224, C2908147, C627668, C2887324, C5143397, C2689642, C141588 ({cm.PRICES_ON});
jlcpcb.com/help/article/pcb-assembly-price; jlcpcb.com/news/discounts-for-large-pcb-orders; exchange-rates.org (USD/LKR);
customs.gov.lk "Computation of import levies" (2026); KPMG, Sri Lanka budget 2026 tax proposals; teamrapidtooling.com (mould costs);
besomi.com (603450 LiPo retail price).</p>

<h2><span class="num">12</span>Verification plan</h2>
<ul>
<li>Fit and tune the antenna (C16-C19, R15), then repeat modes 2 and 4 of the power-test build: the field current and the
wake-up mode average should both fall.</li>
<li>Read <code>dbg_nfc_false_wakes</code> after an hour of idle with no card near (plug in USB to keep the MCU awake for the probe) and replace
the assumed {T['false_per_min']:g} per minute.</li>
<li>Integrate one tap and one idle minute with a current-integrating meter (Nordic PPK2 or similar), with no debugger attached, and compare
with {ev_t['tap']/1000:.1f} mC and {idle_t:.0f} uA.</li>
</ul>
</body></html>
"""

# Typographic units: written as uA/uC/x above so the source stays ASCII.
html = html.replace("uAh", "µAh")
html = re.sub(r"(?<=[\s\d(~>])uA\b", "µA", html)
html = re.sub(r"(?<=[\s\d(~>])uC\b", "µC", html)
html = re.sub(r"(?<=\d) us\b", " µs", html)
html = html.replace("MOhm", "MΩ").replace("470 R", "470 Ω")
html = re.sub(r"(?<=[\w)%]) x (?=[\w(])", " × ", html)

with open(OUT_HTML, "w") as fh:
    fh.write(html)
print(f"idle {idle_t:.2f} uA, off {off_t:.2f} uA, tap {ev_t['tap']:.0f} uC, lecture {lect_t:.1f} uAh, "
      f"life4 {life4_t:.1f}/{life4_p:.1f} mo, log {LOG_CAP} records, {LECT100} lectures of 100")

"""Build Firmware/docs/QUICK_GUIDE.pdf. The light/buzz timelines are drawn from the
firmware's feedback tables (App/Src/feedback.c); update P below if those change.

Usage: python3 make_quick_guide.py quick_guide.html
       google-chrome --headless=new --no-pdf-header-footer --print-to-pdf=QUICK_GUIDE.pdf quick_guide.html"""
import sys

G, R, V = 1, 2, 4

def blinks(n, out, ms):
    return [(out, ms), (0, ms)] * n

P = {
    "accepted":  [(G | V, 90), (G, 160)],
    "duplicate": [(G | V, 60), (0, 90), (G | V, 60)],
    "unknown":   [(R | V, 450)],
    "lecture":   [(G | V, 80), (G, 120), (G | V, 80), (G, 120), (G | V, 80), (G, 400)],
    "power_on":  [(G | V, 120), (G, 180)],
    "power_off": [(R | V, 250), (R, 450)],
    "status_ok": blinks(2, G, 150),
    "hold":      [(V, 60)],
    "saved":     [(G | V, 90), (G, 90), (G | V, 90), (G, 600)],
    "rejected":  [(R | V, 150), (R, 100), (R | V, 150), (R, 100), (R | V, 150), (R, 400)],
    "lowbatt":   blinks(5, R, 150),
    "error":     blinks(4, R | V, 100),
}

SCALE = 0.13  # px per ms; the longest pattern (1500 ms) is 240 px wide
W = 205

def svg(steps):
    x = 4.0
    led, buzz = [], []
    for out, ms in steps:
        w = ms * SCALE
        if out & G:
            led.append(f'<rect x="{x:.1f}" y="3" width="{w:.1f}" height="11" rx="3" fill="#1fa855"/>')
        if out & R:
            led.append(f'<rect x="{x:.1f}" y="3" width="{w:.1f}" height="11" rx="3" fill="#d93434"/>')
        if out & V:
            # zigzag across the buzz span
            pts, n = [], max(2, int(w / 4))
            for i in range(n + 1):
                px = x + w * i / n
                py = 19 if i % 2 == 0 else 26
                pts.append(f"{px:.1f},{py}")
            buzz.append(f'<polyline points="{" ".join(pts)}" fill="none" stroke="#333" stroke-width="1.6" stroke-linejoin="round"/>')
        x += w
    track = (f'<line x1="4" y1="8.5" x2="{W-4}" y2="8.5" stroke="#e3e3e3" stroke-width="11" stroke-linecap="round"/>'
             f'<line x1="4" y1="22.5" x2="{W-4}" y2="22.5" stroke="#ececec" stroke-width="1"/>')
    return (f'<svg width="{W}" height="30" viewBox="0 0 {W} 30" xmlns="http://www.w3.org/2000/svg">'
            f'{track}{"".join(led)}{"".join(buzz)}</svg>')

def row(key, title, meaning, action=""):
    act = f'<div class="act">{action}</div>' if action else ""
    return (f'<tr><td class="pat">{svg(P[key])}</td>'
            f'<td><div class="sig">{title}</div></td>'
            f'<td><div class="mean">{meaning}</div>{act}</td></tr>')

def table(heading, rows):
    return (f'<h3>{heading}</h3><table class="ind"><colgroup><col style="width:212px">'
            f'<col style="width:190px"><col></colgroup>{"".join(rows)}</table>')

tap = table("When a card is tapped", [
    row("accepted", "Green + one short buzz", "<b>Recorded.</b> A registered card."),
    row("unknown", "Red + one long buzz", "<b>Recorded, but the card is not registered.</b>",
        "Register it in the companion app (see page 2). The tap is kept."),
    row("duplicate", "Green + two quick buzzes", "<b>Already recorded</b> in this lecture. Not counted again.",
        "Nothing to do. The student is already marked present."),
    row("error", "Red flashing 4 times, buzzing", "<b>Not recorded: the memory is full.</b>",
        "Plug into the computer and start a lecture from the app (this empties the device)."),
])

btn = table("When you use the button", [
    row("power_on", "Green + short buzz", "<b>Switched on</b> and ready for cards."),
    row("status_ok", "Two green blinks", "<b>Battery is fine</b> (after a quick tap of the button)."),
    row("hold", "A buzz while you hold, no light", "<b>2 seconds reached.</b> Let go now to start a new lecture."),
    row("lecture", "Green + three quick buzzes", "<b>A new lecture has started.</b> Every card counts again."),
    row("power_off", "Red + long buzz", "<b>Switching off.</b> Also happens by itself after 3 minutes without a tap."),
])

pc = table("After using a computer", [
    row("saved", "Green + two buzzes", "<b>Settings accepted.</b> The reader is on and taking cards."),
    row("rejected", "Red + three buzzes", "<b>Settings refused.</b> Nothing changed.",
        "Open <code>STATUS.TXT</code> on the drive: it says which line is wrong."),
])

warn = table("Warnings", [
    row("lowbatt", "Red blinking 5 times, no buzz", "<b>Battery low.</b> If it was already very low the device then switches off.",
        "Charge it with the USB-C cable."),
    row("error", "Red flashing 4 times + buzzes, at switch-on", "<b>The card reader did not start.</b>",
        "Switch off (hold 5 s) and on again. If it repeats, get it serviced."),
])

legend = (f'<div class="legend"><svg width="250" height="40" viewBox="0 0 250 40">'
          f'<line x1="4" y1="11" x2="246" y2="11" stroke="#e3e3e3" stroke-width="14" stroke-linecap="round"/>'
          f'<rect x="4" y="4" width="70" height="14" rx="3" fill="#1fa855"/>'
          f'<rect x="90" y="4" width="70" height="14" rx="3" fill="#d93434"/>'
          f'<polyline points="176,27 180,35 184,27 188,35 192,27 196,35 200,27 204,35 208,27" fill="none" stroke="#333" stroke-width="1.6"/>'
          f'</svg><div><b>How to read the pictures:</b> each strip shows about 1.5 seconds from left to right. '
          f'The top bar is the light (<span class="g">green</span> or <span class="r">red</span>), '
          f'the zig-zag underneath is the vibration buzz.</div></div>')

html = f"""<!doctype html>
<html><head><meta charset="utf-8"><title>Attendance Logger Quick Guide</title>
<style>
@page {{ size: A4; margin: 11mm 14mm 10mm 14mm; }}
* {{ box-sizing: border-box; }}
body {{ font-family: "Inter", "Segoe UI", "Helvetica Neue", Arial, sans-serif; color:#1d1d1f; font-size: 9.6pt; line-height:1.36; margin:0; }}
h1 {{ font-size: 21pt; margin: 0 0 2px; letter-spacing:-0.3px; }}
.sub {{ color:#555; font-size: 10.5pt; margin-bottom: 6px; }}
h2 {{ font-size: 12.5pt; margin: 11px 0 5px; padding-bottom: 3px; border-bottom: 2px solid #1d1d1f; }}
h3 {{ font-size: 9.4pt; text-transform: uppercase; letter-spacing: .6px; color:#444; margin: 9px 0 3px; }}
p {{ margin: 4px 0 6px; }}
code {{ font-family: "JetBrains Mono", Consolas, monospace; font-size: 9pt; background:#f1f1f3; padding: 0 3px; border-radius:3px; }}
.g {{ color:#14843f; font-weight:600; }} .r {{ color:#c02525; font-weight:600; }}
table.ind {{ width:100%; border-collapse: collapse; table-layout: fixed; }}
table.ind td {{ border-top: 1px solid #e4e4e7; padding: 1px 6px; line-height:1.25; vertical-align: middle; }}
table.ind tr:last-child td {{ border-bottom: 1px solid #e4e4e7; }}
td.pat {{ padding-left:0 !important; }}
.sig {{ font-weight: 600; font-size: 9.2pt; }}
.mean {{ font-size: 9.4pt; }}
.act {{ font-size: 8.7pt; color:#555; margin-top: 1px; }}
.legend {{ display:flex; gap:12px; align-items:center; background:#f6f6f8; border-radius:8px; padding:4px 10px; font-size:9.4pt; margin: 6px 0 2px; }}
.steps {{ display:grid; grid-template-columns: repeat(4, 1fr); gap: 8px; margin: 6px 0 4px; }}
.step {{ border:1px solid #dcdce0; border-radius:8px; padding:5px 8px; }}
.step .n {{ display:inline-block; width:20px; height:20px; border-radius:50%; background:#1d1d1f; color:#fff; text-align:center; font-weight:700; font-size:9.5pt; line-height:20px; margin-bottom:2px; }}
.step b {{ display:block; margin-bottom:2px; }}
.step {{ font-size: 9pt; line-height:1.3; }}
table.plain {{ width:100%; border-collapse: collapse; margin: 4px 0; }}
table.plain th {{ text-align:left; font-size:9pt; text-transform:uppercase; letter-spacing:.5px; color:#555; border-bottom:1.5px solid #1d1d1f; padding:4px 6px; }}
table.plain td {{ border-bottom:1px solid #e4e4e7; padding:3px 6px; vertical-align: top; font-size: 9.3pt; }}
table.plain td:first-child {{ font-weight:600; width: 30%; }}
.note {{ border-left: 4px solid #e0a100; background:#fff8e5; padding: 6px 10px; border-radius: 0 6px 6px 0; font-size:9.6pt; margin: 8px 0; }}
.tip {{ border-left: 4px solid #1fa855; background:#eefaf2; padding: 6px 10px; border-radius: 0 6px 6px 0; font-size:9.6pt; margin: 8px 0; }}
.two {{ display:grid; grid-template-columns: 1fr 1fr; gap: 14px; }}
ol, ul {{ margin: 4px 0 6px; padding-left: 18px; }} li {{ margin: 2px 0; }}
.page {{ page-break-after: always; }}
.foot {{ color:#777; font-size: 8.2pt; margin-top: 6px; border-top:1px solid #e4e4e7; padding-top:5px; }}
</style></head><body>

<div class="page">
<h1>Attendance Logger: Quick Guide</h1>
<div class="sub">For lecturers and staff who take attendance with the card reader.</div>

<p>Students tap their ID card on the reader. The device stores the card number and time, and answers with a <b>light</b> and a <b>buzz</b>. The companion app on the computer later shows who attended each lecture.</p>

<h2>Taking attendance in four steps</h2>
<div class="steps">
  <div class="step"><span class="n">1</span><b>Switch on</b>Press the button once. Green light and a short buzz.</div>
  <div class="step"><span class="n">2</span><b>Tap cards</b>Hold each card flat on the reader for a moment until it buzzes.</div>
  <div class="step"><span class="n">3</span><b>Next lecture</b>Hold the button until it buzzes (2 s), then let go. Or start it from the app.</div>
  <div class="step"><span class="n">4</span><b>Plug in</b>Connect USB-C to the computer and open the companion app to see the results.</div>
</div>

<h2>What the lights and buzzes mean</h2>
{legend}
{tap}
{btn}
{pc}
{warn}
</div>

<div>
<h2>The power button</h2>
<table class="plain">
<tr><th>What you do</th><th>What happens</th></tr>
<tr><td>Press once (device off)</td><td>Switches on: green light and a short buzz.</td></tr>
<tr><td>Quick tap (device on)</td><td>Shows the battery: two green blinks = fine, red blinking 5 times = low.
  While the device is a USB drive on a computer, a tap ends that and starts taking cards instead.</td></tr>
<tr><td>Hold 2 s until it buzzes, then let go</td><td><b>Starts the next lecture</b> (green + three quick buzzes). It keeps the module name and counts the
  lecture on: <i>Lecture 4</i> becomes <i>Lecture 5</i>.</td></tr>
<tr><td>Keep holding for 5 s</td><td>Switches off (red + long buzz). No lecture is started.</td></tr>
</table>
<div class="tip">On battery the device switches itself off after <b>3 minutes</b> without a tap. Press the button to wake it.
Your records are safe: they are kept when it switches off.</div>

<div class="two">
<div>
<h2>Lectures and double taps</h2>
<ul>
<li>Each student counts <b>once per lecture</b>, even if the device slept in between. A second tap gets two quick buzzes.</li>
<li>When one lecture ends and the next begins, start a new lecture (hold the button 2 s, or use the app). Then everyone counts again.</li>
<li>If you never start a lecture, a card already recorded in the last 6 hours still counts only once.</li>
</ul>

<h2>Using it with the computer</h2>
<ol>
<li>Plug the device in with the USB-C cable. A drive called <b>ATTENDANCE</b> appears. The reader is <b>off</b> while it is a drive.</li>
<li>Open the companion app. It reads the taps and shows attendance for each lecture.</li>
<li>To start a lecture, use the <b>Lecture</b> tab in the app. It saves all taps, empties the device, sets the clock and ejects the drive.</li>
<li>The device answers green + two buzzes and starts taking cards. <b>The cable can stay in</b>: it charges while it works.</li>
</ol>
<p>Without the app: eject the drive, or tap the button once, to switch the reader back on.</p>
</div>

<div>
<h2>Registering a new student card</h2>
<ol>
<li>Tap the new card on the device. It shows <span class="r">red</span> with a long buzz, but the tap <b>is recorded</b>.</li>
<li>Plug the device in and open the app. The card appears under <i>New cards</i> on the Students tab.</li>
<li>Press <b>Register</b> and fill in the student's details.</li>
<li>Press <b>Send cards to device</b>, then eject. Green + two buzzes means it was stored.</li>
</ol>
<p>From then on that card shows <span class="g">green</span>.</p>

<h2>Charging</h2>
<ul>
<li>Charge with any USB-C charger or computer port. It keeps taking cards while it charges.</li>
<li>The clock keeps running while the device is off. If the battery ever goes completely flat, set the clock again (the app does it when you start a lecture).</li>
</ul>
</div>
</div>

<h2>If something is not right</h2>
<table class="plain">
<tr><th>Problem</th><th>What to do</th></tr>
<tr><td>Nothing happens when a card is tapped</td><td>Press the button: the device may have switched itself off. If it is plugged into a computer, the reader is off; eject the drive or tap the button.</td></tr>
<tr><td>Card does not read</td><td>Hold the card flat and still on the reader for about a second. Keep it away from phones and other cards in the same wallet.</td></tr>
<tr><td>Every tap shows red</td><td>The device does not have the latest card list. Press <b>Send cards to device</b> in the app.</td></tr>
<tr><td>The battery runs low quickly</td><td>Charge it fully with the USB-C cable. A tap of the button shows the battery: two green blinks = fine.</td></tr>
<tr><td>Times in the records are wrong</td><td>Set the clock: start a lecture from the app, or use <b>Set the device clock</b>.</td></tr>
</table>



<div class="foot">Attendance Logger, firmware quick guide. Full details: <code>Firmware/docs/USER_GUIDE.md</code> and <code>Companion/README.md</code>.</div>
</div>
</body></html>
"""

open(sys.argv[1], "w", encoding="utf-8").write(html)

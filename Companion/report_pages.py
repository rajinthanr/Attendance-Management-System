"""
Printable report pages, and PDFs made from them.

Each report is one standalone HTML page with its own style sheet, so it prints
the same from the browser's Print command as it does as a PDF. PDFs are made by
asking the PC's own Chrome or Edge to print the page: they draw every script
correctly (Sinhala and Tamil names included), which a hand-made PDF with the
built-in fonts could not.
"""
import html
import os
import shutil
import subprocess
import sys
import tempfile
import time

import attendance_db as D

_CSS = """
:root { color-scheme: light; }
* { box-sizing: border-box; }
body { font-family: "Segoe UI", system-ui, -apple-system, "Noto Sans", "Noto Sans Sinhala", "Noto Sans Tamil", Arial, sans-serif;
       color: #111; margin: 0; padding: 18mm 14mm; font-size: 11pt; line-height: 1.35; }
h1 { font-size: 19pt; margin: 0 0 2pt; }
h2 { font-size: 13pt; margin: 14pt 0 4pt; }
.sub { color: #444; margin: 0 0 10pt; }
.meta { display: flex; flex-wrap: wrap; gap: 6pt 24pt; margin: 8pt 0 12pt; }
.meta div b { display: block; font-size: 17pt; line-height: 1.1; }
.meta div span { color: #555; font-size: 9pt; text-transform: uppercase; letter-spacing: .04em; }
table { border-collapse: collapse; width: 100%; font-size: 10pt; }
th, td { border: 1px solid #999; padding: 3pt 6pt; text-align: left; vertical-align: top; }
th { background: #eee; font-size: 8.5pt; text-transform: uppercase; letter-spacing: .03em; }
td.c, th.c { text-align: center; }
td.num { font-variant-numeric: tabular-nums; white-space: nowrap; }
td.p { background: #d8f0de; text-align: center; font-weight: 700; }
td.a { background: #f6d9d6; text-align: center; color: #8a1f17; }
td.low { color: #b3261e; font-weight: 700; }
tr { break-inside: avoid; }
.sign { display: flex; gap: 40pt; margin-top: 30pt; }
.sign div { flex: 1; border-top: 1px solid #000; padding-top: 3pt; font-size: 9pt; }
.foot { margin-top: 14pt; color: #666; font-size: 8.5pt; }
.empty { color: #666; font-style: italic; }
@page { size: A4 portrait; margin: 0; }
@page wide { size: A4 landscape; margin: 0; }
body.wide { page: wide; }
@media screen { body { max-width: 1000px; margin: 0 auto; } .noprint { display: block; } }
@media print { .noprint { display: none; } }
.noprint { background: #eef4fb; border: 1px solid #b6cfe9; border-radius: 8px; padding: 8pt 12pt; margin-bottom: 14pt; }
"""

_PRINT_HINT = ('<div class="noprint">This is a printable report. Use your browser\'s Print command '
               '(Ctrl+P) and choose <b>Save as PDF</b> or a printer.</div>')


def _e(x):
    return html.escape(str(x if x is not None else ""))


def _page(title, body, wide=False):
    return ('<!doctype html><html lang="en"><head><meta charset="utf-8"><title>%s</title><style>%s</style></head>'
            '<body class="%s">%s%s<div class="foot">Generated %s by Attendance Logger</div>'
            '<script>if(location.search.indexOf("print=1")>=0)window.addEventListener("load",function(){setTimeout(print,250)})</script>'
            '</body></html>'
            % (_e(title), _CSS, "wide" if wide else "", _PRINT_HINT, body, time.strftime("%Y-%m-%d %H:%M")))


def _table(head, rows, classes=None):
    classes = classes or []
    out = ["<table><thead><tr>%s</tr></thead><tbody>" % "".join("<th>%s</th>" % _e(h) for h in head)]
    if not rows:
        out.append('<tr><td colspan="%d" class="empty">None</td></tr>' % len(head))
    for r in rows:
        out.append("<tr>%s</tr>" % "".join('<td%s>%s</td>' % (' class="%s"' % classes[i] if i < len(classes) and classes[i] else "", _e(c))
                                           for i, c in enumerate(r)))
    out.append("</tbody></table>")
    return "".join(out)


def lecture_page(att):
    l, c = att["lecture"], att["counts"]
    meta = ('<div class="meta"><div><b>%d</b><span>present</span></div><div><b>%d</b><span>absent</span></div>'
            '<div><b>%s%%</b><span>of %d enrolled</span></div>%s%s</div>'
            % (c["present_enrolled"], c["absent"], c["percent"], c["enrolled"],
               '<div><b>%d</b><span>not enrolled in this module</span></div>' % c["present_other"] if c["present_other"] else "",
               '<div><b>%d</b><span>unregistered cards</span></div>' % c["unregistered"] if c["unregistered"] else ""))
    body = ["<h1>%s</h1>" % _e(l["title"]),
            '<p class="sub">%s%s &middot; %s &middot; %s to %s</p>' % (
                _e(l["module_code"]), (" " + _e(_module_title(att))) if _module_title(att) else "", _e(l["date"]),
                _e(l["start_text"][11:16]), _e(l["end_text"][11:16])), meta,
            "<h2>Present (%d)</h2>" % len(att["present"]),
            _table(["Name", "Student no", "Department", "Card", "Arrived"],
                   [[p["name"], p["student_no"], p["department"], D.fmt_card(p["card_id"]), p["time"]] for p in att["present"]],
                   ["", "", "", "num", "num"]),
            "<h2>Absent (%d)</h2>" % len(att["absent"]),
            _table(["Name", "Student no", "Department", "Card"],
                   [[a["name"], a["student_no"], a["department"], D.fmt_card(a["card_id"])] for a in att["absent"]],
                   ["", "", "", "num"])]
    if att["present_other"]:
        body += ["<h2>Present but not enrolled in %s (%d)</h2>" % (_e(l["module_code"]), len(att["present_other"])),
                 _table(["Name", "Student no", "Department", "Card", "Arrived"],
                        [[p["name"], p["student_no"], p["department"], D.fmt_card(p["card_id"]), p["time"]] for p in att["present_other"]],
                        ["", "", "", "num", "num"])]
    if att["unregistered"]:
        body += ["<h2>Unregistered cards (%d)</h2>" % len(att["unregistered"]),
                 _table(["Card", "Time"], [[D.fmt_card(u["card_id"]), u["time"]] for u in att["unregistered"]], ["num", "num"])]
    body.append('<div class="sign"><div>Lecturer</div><div>Date</div></div>')
    return _page("%s %s" % (l["module_code"], l["title"]), "".join(body))


def _module_title(att):
    return att.get("module_title", "")


def module_page(rep):
    m, s = rep["module"], rep["summary"]
    rng = ""
    if rep["from"] or rep["to"]:
        rng = " &middot; %s to %s" % (_e(rep["from"] or "start"), _e(rep["to"] or "now"))
    head = ["Name", "Student no", "Card"] + ["%s %s" % (l["date"][5:], l["title"]) for l in rep["lectures"]] + ["Present", "%"]
    rows = []
    for st in rep["students"]:
        cells = ['<td>%s</td><td>%s</td><td class="num">%s</td>' % (_e(st["name"]), _e(st["student_no"]), D.fmt_card(st["card_id"]))]
        cells += ['<td class="%s">%s</td>' % ("p" if f else "a", "P" if f else "&ndash;") for f in st["flags"]]
        cells.append('<td class="c num">%d/%d</td><td class="c num%s">%s%%</td>' % (
            st["count"], s["lectures"], " low" if st["percent"] < 75 else "", st["percent"]))
        rows.append("<tr>%s</tr>" % "".join(cells))
    table = ('<table><thead><tr>%s</tr></thead><tbody>%s</tbody></table>'
             % ("".join("<th%s>%s</th>" % (' class="c"' if i >= 3 else "", _e(h)) for i, h in enumerate(head)),
                "".join(rows) or '<tr><td colspan="%d" class="empty">No enrolled students</td></tr>' % len(head)))
    body = ["<h1>%s %s</h1>" % (_e(m["code"]), _e(m["title"])),
            '<p class="sub">Attendance report%s</p>' % rng,
            '<div class="meta"><div><b>%d</b><span>lectures</span></div><div><b>%d</b><span>students</span></div>'
            '<div><b>%s%%</b><span>average attendance</span></div><div><b>%d</b><span>below 75%%</span></div></div>'
            % (s["lectures"], s["students"], s["average"], s["below_75"]),
            table, '<div class="sign"><div>Lecturer</div><div>Date</div></div>']
    return _page("%s attendance" % m["code"], "".join(body), wide=len(rep["lectures"]) > 6)


def student_page(rep):
    st = rep["student"]
    body = ["<h1>%s</h1>" % _e(st["name"]),
            '<p class="sub">Card %s &middot; Student no %s &middot; %s</p>' % (
                D.fmt_card(st["card_id"]), _e(st["student_no"] or "-"), _e(st["department"] or "no department")),
            '<div class="meta"><div><b>%d</b><span>taps recorded</span></div><div><b>%s</b><span>last tap</span></div></div>'
            % (rep["taps"], _e(rep["last_tap"] or "never"))]
    for m in rep["modules"]:
        body.append("<h2>%s %s &mdash; %d of %d lectures (%s%%)</h2>" % (_e(m["code"]), _e(m["title"]), m["attended"], m["total"], m["percent"]))
        body.append(_table(["Date", "Lecture", "Attended", "Arrived"],
                           [[l["date"], l["title"], "Yes" if l["attended"] else "No", l["time"]] for l in m["lectures"]],
                           ["num", "", "", "num"]))
    if not rep["modules"]:
        body.append('<p class="empty">Not enrolled in any module.</p>')
    return _page("%s attendance" % st["name"], "".join(body))


# --------------------------------------------------------------------------
# PDF, by way of the PC's own browser
# --------------------------------------------------------------------------

def find_browser():
    """Path of a Chrome, Edge or Chromium that can print to PDF, or None."""
    cands = [os.environ.get("ATTENDANCE_BROWSER")]
    if sys.platform.startswith("win"):
        for base in (os.environ.get("ProgramFiles"), os.environ.get("ProgramFiles(x86)"), os.environ.get("LocalAppData")):
            if base:
                cands += [os.path.join(base, "Google", "Chrome", "Application", "chrome.exe"),
                          os.path.join(base, "Microsoft", "Edge", "Application", "msedge.exe")]
    elif sys.platform == "darwin":
        cands += ["/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
                  "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
                  "/Applications/Chromium.app/Contents/MacOS/Chromium"]
    for name in ("google-chrome", "google-chrome-stable", "chromium", "chromium-browser", "microsoft-edge", "chrome", "msedge"):
        p = shutil.which(name)
        if p:
            cands.append(p)
    for c in cands:
        if c and os.path.isfile(c):
            return c
    return None


def render_pdf(page_html, timeout=40):
    """PDF bytes for @page_html, or raises RuntimeError with a message fit to show the user."""
    browser = find_browser()
    if not browser:
        raise RuntimeError("PDF needs Chrome or Edge on this computer. Use Print and choose Save as PDF instead.")
    work = tempfile.mkdtemp(prefix="att-pdf-")
    try:
        src = os.path.join(work, "report.html")
        out = os.path.join(work, "report.pdf")
        with open(src, "w", encoding="utf-8") as f:
            f.write(page_html)
        url = "file:///" + src.replace("\\", "/").lstrip("/")
        args = [browser, "--headless=new", "--disable-gpu", "--no-sandbox", "--no-first-run",
                "--no-default-browser-check", "--no-pdf-header-footer", "--user-data-dir=" + os.path.join(work, "profile"),
                "--print-to-pdf=" + out, url]
        try:
            subprocess.run(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=timeout, check=False)
        except subprocess.TimeoutExpired:
            raise RuntimeError("The browser took too long to make the PDF.")
        if not os.path.isfile(out) or os.path.getsize(out) < 200:
            raise RuntimeError("The browser could not make the PDF. Use Print and choose Save as PDF instead.")
        with open(out, "rb") as f:
            return f.read()
    finally:
        shutil.rmtree(work, ignore_errors=True)

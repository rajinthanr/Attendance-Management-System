"""
Printable report pages, and PDFs made from them.

Each report is one standalone HTML page with its own style sheet, so it prints
the same from the browser's Print command as it does as a PDF. Those PDFs are
made by asking the PC's own Chrome or Edge to print the page: they draw every
script correctly (Sinhala and Tamil names included), which a hand-made PDF with
the built-in fonts could not.

Plain tables (a lecture's list, every tap, the students) are written as PDF
here, with nothing but the standard library: table_pdf(). That needs no browser,
so saving a PDF never fails for the want of one; its built-in Helvetica covers
Western European text only, and other letters print as "?".
"""
import html
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unicodedata
import zlib

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

NO_BROWSER = "PDF needs Chrome or Edge on this computer. Use Print and choose Save as PDF instead."


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
        raise RuntimeError(NO_BROWSER)
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


# --------------------------------------------------------------------------
# PDF, written here: a ruled table in the built-in Helvetica, no browser needed
# --------------------------------------------------------------------------

PAGE_W, PAGE_H = 595.28, 841.89      # A4 portrait, in points
_MARGIN = 42.0
_ROW_H = 16.0
_TEXT_PT = 9.0
_PAD = 4.0                           # space between a column's rule and its text
# Advance widths (1/1000 em) of the printable ASCII characters, from Adobe's metrics for the two fonts.
_W_REGULAR = [int(x) for x in (
    "278 278 355 556 556 889 667 191 333 333 389 584 278 333 278 278 556 556 556 556 556 556 556 556 556 556 278 278 "
    "584 584 584 556 1015 667 667 722 722 667 611 778 722 278 500 667 556 833 722 778 667 778 722 667 611 722 667 944 "
    "667 667 611 278 278 278 469 556 333 556 556 500 556 556 278 556 556 222 222 500 222 833 556 556 556 556 333 500 "
    "278 556 500 722 500 500 500 334 260 334 584").split()]
_W_BOLD = [int(x) for x in (
    "278 333 474 556 556 889 722 238 333 333 389 584 278 333 278 278 556 556 556 556 556 556 556 556 556 556 333 333 "
    "584 584 584 611 975 722 722 722 722 667 611 778 722 278 556 722 611 833 722 778 667 778 722 667 611 722 667 944 "
    "667 667 611 333 278 333 584 556 333 556 611 556 611 556 333 611 611 278 278 556 278 889 611 611 611 611 389 556 "
    "333 611 556 778 556 556 500 389 280 389 584").split()]
_ELLIPSIS = "\u2026"                  # in WinAnsi (0x85), 1000 units wide in both fonts


def pdf_text(s):
    """@s as text the built-in fonts can print (WinAnsi): accents outside it are dropped where that leaves a
    letter (o with a double acute -> o), control characters become spaces, and anything else becomes '?'."""
    out = []
    for ch in str("" if s is None else s):
        if ch < " " or ch == "\x7f":
            out.append(" ")
            continue
        try:
            ch.encode("cp1252")
            out.append(ch)
            continue
        except UnicodeEncodeError:
            pass
        base = "".join(c for c in unicodedata.normalize("NFKD", ch) if not unicodedata.combining(c))
        try:
            base.encode("cp1252")
            out.append(base or "?")
        except UnicodeEncodeError:
            out.append("?")
    return "".join(out)


def _char_w(ch, bold):
    o = ord(ch)
    if 32 <= o <= 126:
        return (_W_BOLD if bold else _W_REGULAR)[o - 32]
    if ch == _ELLIPSIS:
        return 1000
    base = unicodedata.normalize("NFKD", ch)[:1]          # an accented letter is as wide as its letter
    if base and 32 <= ord(base) <= 126:
        return (_W_BOLD if bold else _W_REGULAR)[ord(base) - 32]
    return 667                                           # the wider side of the rest, so text is never cut too late


def text_width(s, size, bold=False):
    """Width in points of @s (already pdf_text()) in Helvetica at @size."""
    return sum(_char_w(c, bold) for c in s) * size / 1000.0


def fit_text(s, width, size, bold=False):
    """@s cut to fit @width points, ending in an ellipsis when it was cut."""
    s = pdf_text(s)
    if text_width(s, size, bold) <= width:
        return s
    room = width - text_width(_ELLIPSIS, size, bold)
    out, used = "", 0.0
    for c in s:
        w = _char_w(c, bold) * size / 1000.0
        if used + w > room:
            break
        out += c
        used += w
    return out.rstrip() + _ELLIPSIS if room > 0 else ""


def _pdf_str(s):
    """A PDF literal string of WinAnsi text."""
    b = s.encode("cp1252", errors="replace")
    return b"(" + b.replace(b"\\", b"\\\\").replace(b"(", b"\\(").replace(b")", b"\\)") + b")"


def _col_widths(headers, rows, avail, size):
    """Each column's width: what its text needs (headers in bold), shared out to fill @avail; a column that
    would need more than its share gives way, and its text is cut to fit."""
    n = len(headers)
    need = [text_width(pdf_text(h), size, True) for h in headers]
    for r in rows[:2000]:                                # enough rows to judge by, without measuring them all
        for i in range(n):
            v = r[i] if i < len(r) else ""
            need[i] = max(need[i], text_width(pdf_text(v), size))
    need = [min(w + 2 * _PAD + 2, avail * 0.4) for w in need]      # no column may crowd out all the others
    if sum(need) <= avail:
        extra = (avail - sum(need)) / n
        return [w + extra for w in need]
    # Too wide: narrow columns keep what they need, the wide ones share the rest.
    fair, fixed, wide = avail / n, [], []
    for i, w in enumerate(need):
        (fixed if w <= fair else wide).append(i)
    left = avail - sum(need[i] for i in fixed)
    total_wide = sum(need[i] for i in wide) or 1.0
    out = list(need)
    for i in wide:
        out[i] = max(fair * 0.5, left * need[i] / total_wide)
    scale = avail / sum(out)
    return [w * scale for w in out]


def table_pdf(title, subtitle, headers, rows, generated=None):
    """
    A PDF (bytes, PDF 1.4) of a ruled table: A4 portrait, the built-in Helvetica, @title and @subtitle and
    a "Generated" line on the first page, the header row repeated at the top of every page, and page numbers.
    @rows is a list of lists of cells (any values, shown as text); a cell too wide for its column is cut.
    Text outside Western European letters prints as "?" (pdf_text()).
    """
    generated = generated or time.strftime("%Y-%m-%d %H:%M")
    headers = [str(h) for h in headers]
    rows = [["" if c is None else str(c) for c in r] for r in rows]
    left, right = _MARGIN, PAGE_W - _MARGIN
    widths = _col_widths(headers, rows, right - left, _TEXT_PT)
    bottom = _MARGIN + 18                                # the page number sits below this
    pages, ops = [], []

    def text(x, y, s, size, bold=False, grey=0.0):
        ops.append(b"BT /%s %.1f Tf %.3f g %.2f %.2f Td %s Tj ET" % (b"F2" if bold else b"F1", size, grey, x, y, _pdf_str(s)))

    def rule(y, grey=0.75, width=0.5):
        ops.append(b"%.3f G %.2f w %.2f %.2f m %.2f %.2f l S" % (grey, width, left, y, right, y))

    def header_row(y):
        ops.append(b"0.925 g %.2f %.2f %.2f %.2f re f" % (left, y - _ROW_H, right - left, _ROW_H))
        x = left
        for h, w in zip(headers, widths):
            text(x + _PAD, y - _ROW_H + 5, fit_text(h, w - 2 * _PAD, _TEXT_PT, True), _TEXT_PT, True, 0.15)
            x += w
        rule(y - _ROW_H, 0.4, 0.8)
        return y - _ROW_H

    def new_page(first):
        y = PAGE_H - _MARGIN
        if first:
            text(left, y - 16, fit_text(title, right - left, 16, True), 16, True)
            y -= 24
            if subtitle:
                text(left, y - 10, fit_text(subtitle, right - left, 10), 10, False, 0.3)
                y -= 15
            text(left, y - 9, fit_text("Generated %s by Attendance Logger" % generated, right - left, 8.5), 8.5,
                 False, 0.45)
            y -= 22
        else:
            text(left, y - 10, fit_text(title, right - left, 10, True), 10, True, 0.3)
            y -= 20
        return header_row(y)

    y = new_page(True)
    if not rows:
        text(left + _PAD, y - _ROW_H + 5, "None", _TEXT_PT, False, 0.45)
    for r in rows:
        if y - _ROW_H < bottom:
            pages.append(ops)
            ops = []
            y = new_page(False)
        x = left
        for i, w in enumerate(widths):
            v = r[i] if i < len(r) else ""
            if v:
                text(x + _PAD, y - _ROW_H + 5, fit_text(v, w - 2 * _PAD, _TEXT_PT), _TEXT_PT)
            x += w
        y -= _ROW_H
        rule(y)
    pages.append(ops)

    # Objects: 1 catalog, 2 page tree, 3 and 4 the fonts, 5 the document info, then a page and its contents each.
    n = len(pages)
    objs = [b"<< /Type /Catalog /Pages 2 0 R >>",
            b"<< /Type /Pages /Kids [%s] /Count %d >>" % (b" ".join(b"%d 0 R" % (6 + 2 * i) for i in range(n)), n),
            b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
            b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>",
            b"<< /Title <%s> /Producer (Attendance Logger) /CreationDate (D:%s) >>"      # the title in UTF-16
            % (("\ufeff" + str(title)).encode("utf-16-be").hex().upper().encode(), time.strftime("%Y%m%d%H%M%S").encode())]
    for i, page_ops in enumerate(pages):
        ops = page_ops[:]
        foot = fit_text(title, (right - left) * 0.7, 8)
        ops.append(b"BT /F1 8 Tf 0.45 g %.2f %.2f Td %s Tj ET" % (left, _MARGIN, _pdf_str(foot)))
        label = "Page %d of %d" % (i + 1, n)
        ops.append(b"BT /F1 8 Tf 0.45 g %.2f %.2f Td %s Tj ET" % (right - text_width(label, 8), _MARGIN, _pdf_str(label)))
        data = zlib.compress(b"\n".join(ops))
        objs.append(b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.2f %.2f] /Resources << /Font << /F1 3 0 R "
                    b"/F2 4 0 R >> >> /Contents %d 0 R >>" % (PAGE_W, PAGE_H, 7 + 2 * i))
        objs.append(b"<< /Length %d /Filter /FlateDecode >>\nstream\n%s\nendstream" % (len(data), data))
    out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = []
    for i, body in enumerate(objs):
        offsets.append(len(out))
        out += b"%d 0 obj\n%s\nendobj\n" % (i + 1, body)
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f\r\n" % (len(objs) + 1)
    out += b"".join(b"%010d 00000 n\r\n" % o for o in offsets)
    out += b"trailer\n<< /Size %d /Root 1 0 R /Info 5 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, xref)
    return bytes(out)

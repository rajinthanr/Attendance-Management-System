#!/usr/bin/env python3
"""
Attendance Logger companion app.

Run it and a browser page opens that keeps the student database, finds the
device when it is plugged in, reads the taps it recorded, starts lectures, and
makes attendance reports as CSV or PDF. It needs only Python 3.8 or newer,
nothing to install and no internet. It listens on this computer only.

    python attendance_app.py              normal use
    python attendance_app.py --demo       try it with made-up data, no device
    python attendance_app.py --data-dir D:\\Attendance    keep the database elsewhere

The device is a small USB drive called ATTENDANCE holding ATTEND.CSV (one row
per tap: date, time, card number), SETTINGS.CSV and STATUS.TXT. It knows nothing
about people. This program keeps who each card belongs to, their department and
modules, and the lectures, in its own database, and matches the two.
"""
import argparse
import datetime
import json
import mimetypes
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlparse

import attendance_db as D
import report_pages as R

HERE = os.path.dirname(os.path.abspath(__file__))
WEB = os.path.join(HERE, "web")

DEFAULT_PORT = 8765
STATUS_NAME = "STATUS.TXT"
ATTEND_NAME = "ATTEND.CSV"
SETTINGS_NAME = "SETTINGS.CSV"
STATUS_MAGIC = "ATTENDANCE LOGGER"
MAX_SETTINGS_BYTES = 28 * 512         # the window on the device (SETF_MAX_BYTES)
MAX_BODY = 4 * 1024 * 1024            # a pasted class list is the largest thing sent
BACKUPS_KEPT = 14


class ApiError(Exception):
    def __init__(self, code, message):
        Exception.__init__(self, message)
        self.code = code
        self.message = message


# --------------------------------------------------------------------------
# Finding the device
# --------------------------------------------------------------------------

def _windows_roots():
    """Drive roots that could hold the device, without waking empty readers."""
    import ctypes
    kernel32 = ctypes.windll.kernel32
    old = kernel32.SetErrorMode(0x0001)          # SEM_FAILCRITICALERRORS: no "insert a disk" pop-ups
    try:
        mask = kernel32.GetLogicalDrives()
        roots = []
        for i in range(26):
            if mask & (1 << i):
                root = "%s:\\" % chr(ord("A") + i)
                kind = kernel32.GetDriveTypeW(root)
                if kind in (2, 3):               # removable or fixed; never network, CD or RAM disks
                    roots.append(root)
        return roots
    finally:
        kernel32.SetErrorMode(old)


def _unix_roots():
    roots = []
    for base in ("/Volumes", "/media", "/run/media", "/mnt"):
        if not os.path.isdir(base):
            continue
        try:
            for name in sorted(os.listdir(base)):
                p = os.path.join(base, name)
                if os.path.isdir(p):
                    roots.append(p)
                    # /media/<user>/<volume> and /run/media/<user>/<volume>
                    try:
                        for sub in sorted(os.listdir(p)):
                            q = os.path.join(p, sub)
                            if os.path.isdir(q):
                                roots.append(q)
                    except OSError:
                        pass
        except OSError:
            pass
    return roots


def candidate_roots():
    try:
        return _windows_roots() if os.name == "nt" else _unix_roots()
    except Exception:
        return []


def read_text(path, limit=None):
    with open(path, "rb") as f:
        data = f.read() if limit is None else f.read(limit)
    return data.decode("utf-8-sig", errors="replace")


def is_device(root):
    """True when @root holds the three files and STATUS.TXT says what it should."""
    try:
        for name in (STATUS_NAME, ATTEND_NAME, SETTINGS_NAME):
            if not os.path.isfile(os.path.join(root, name)):
                return False
        return read_text(os.path.join(root, STATUS_NAME), 64).lstrip().startswith(STATUS_MAGIC)
    except OSError:
        return False


def find_device(forced=None):
    """The folder of a plugged-in device, or None."""
    if forced:
        return forced if is_device(forced) else None
    for root in candidate_roots():
        if is_device(root):
            return root
    return None


def _quiet_run(args, timeout=30):
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    return subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout,
                          creationflags=flags).returncode == 0


def _eject_windows(root):
    """Lock, dismount and eject the volume: what Explorer's Eject does, without its localised verb."""
    import ctypes
    from ctypes import wintypes
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.CreateFileW.restype = wintypes.HANDLE
    k.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                              wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    k.DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD,
                                  wintypes.LPVOID, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
    k.CloseHandle.argtypes = [wintypes.HANDLE]
    drive = os.path.splitdrive(os.path.abspath(root))[0]
    h = k.CreateFileW("\\\\.\\" + drive, 0xC0000000, 3, None, 3, 0, None)   # read/write, share both, open existing
    if h is None or h == wintypes.HANDLE(-1).value:
        return False
    try:
        done = wintypes.DWORD()

        def ioctl(code, buf=None, size=0):
            return bool(k.DeviceIoControl(h, code, buf, size, None, 0, ctypes.byref(done), None))
        for _ in range(20):                      # FSCTL_LOCK_VOLUME: waits for Explorer to let go
            if ioctl(0x00090018):
                break
            time.sleep(0.25)
        else:
            return False
        if not ioctl(0x00090020):                # FSCTL_DISMOUNT_VOLUME
            return False
        allow = ctypes.c_ubyte(0)
        ioctl(0x002D4804, ctypes.byref(allow), 1)  # IOCTL_STORAGE_MEDIA_REMOVAL: allow
        return ioctl(0x002D4808)                 # IOCTL_STORAGE_EJECT_MEDIA: the SCSI eject the device waits for
    finally:
        k.CloseHandle(h)


def _block_device(root):
    """/dev/... mounted at @root, from /proc/mounts (Linux), or None."""
    try:
        with open("/proc/mounts") as f:
            for line in f:
                parts = line.split()
                if len(parts) > 1 and parts[1].encode().decode("unicode_escape") == root.rstrip("/"):
                    return parts[0]
    except OSError:
        pass
    return None


def eject_drive(root):
    """
    Eject the device's drive, as the system's own Eject does. The device sees it, applies SETTINGS.CSV and
    starts taking attendance with the cable still in. Returns True when the system says it worked.
    """
    try:
        system = platform.system()
        if system == "Windows":
            return _eject_windows(root)
        if system == "Darwin":
            return _quiet_run(["diskutil", "eject", root])
        dev = _block_device(root)
        if dev and shutil.which("udisksctl"):
            if _quiet_run(["udisksctl", "unmount", "--no-user-interaction", "-b", dev]):
                return _quiet_run(["udisksctl", "power-off", "--no-user-interaction", "-b", dev])
        if shutil.which("gio"):
            return _quiet_run(["gio", "mount", "--eject", root])
        if shutil.which("eject"):
            return _quiet_run(["eject", root])
    except (OSError, subprocess.SubprocessError):
        pass
    return False


def volume_label(root):
    if os.name != "nt":
        return os.path.basename(root.rstrip("/")) or root
    try:
        import ctypes
        buf = ctypes.create_unicode_buffer(261)
        if ctypes.windll.kernel32.GetVolumeInformationW(root, buf, 261, None, None, None, None, 0):
            return buf.value
    except Exception:
        pass
    return ""


# --------------------------------------------------------------------------
# The application: a database, and at most one device
# --------------------------------------------------------------------------

class App:
    def __init__(self, db, forced_dir=None, data_dir=None):
        self.db = db
        self.forced_dir = forced_dir
        self.data_dir = data_dir
        self.server = None
        self.lock = threading.RLock()
        self.last_path = None
        self.sig = None
        self.status = None              # the device's STATUS.TXT, parsed
        self.last_sync = {"seq": 0, "new": 0, "read": 0, "skipped": 0, "at": ""}
        self.pc_minus_device = None     # seconds the PC clock is ahead of the device's, as last read

    # ---- the device
    def device_dir(self):
        with self.lock:
            # Keep using the last folder while it still checks out, which is quick.
            if self.last_path and is_device(self.last_path):
                return self.last_path
            self.last_path = find_device(self.forced_dir)
            return self.last_path

    def _sync(self, root, st):
        """Read the taps off the device into the database. Safe to repeat: known taps are skipped."""
        rows, skipped = D.parse_attend(read_text(os.path.join(root, ATTEND_NAME)))
        new = self.db.add_taps(rows, st["device_id"])
        if st["has_lecture"] and st["since"]:
            if self.db.confirm_lecture_start(st["module"], st["lecture"], st["since"]):
                self.db.note_confirmed(st["since"])
            else:
                self.db.adopt_device_lecture(st["module"], st["lecture"], st["since"])
        self.last_sync = {"seq": self.last_sync["seq"] + 1, "new": new, "read": len(rows), "skipped": skipped,
                          "at": time.strftime("%H:%M:%S")}

    def refresh(self, force=False):
        """Look at the device; read its taps if anything changed. Returns the state for the page."""
        with self.lock:
            root = self.device_dir()
            if root:
                try:
                    st = D.parse_status(read_text(os.path.join(root, STATUS_NAME), 1024))
                    stat = os.stat(os.path.join(root, ATTEND_NAME))
                    sig = (root, stat.st_size, int(stat.st_mtime), st["records"], st["last_card"], st["last_tap"],
                           st["module"], st["lecture"], st["since"])
                    if force or sig != self.sig:
                        self._sync(root, st)
                        self.sig = sig
                    self.status = st
                    if st["clock"] is not None:
                        self.pc_minus_device = D.now_ts() - st["clock"]
                except OSError:
                    root = None
                    self.sig = None
            if not root:
                self.sig = None
                self.status = None
            return self.state(root)

    def state(self, root):
        st = self.status if root else None
        out = {"connected": bool(root), "demo": bool(self.forced_dir), "platform": platform.system(),
               "now": time.strftime("%Y-%m-%d %H:%M:%S"), "pdf": R.find_browser() is not None,
               "counts": self.db.counts(), "sync": self.last_sync, "current_lecture": self.db.current_lecture(),
               "db_path": getattr(self.db, "path", ""), "last_sync_db": self.db.last_sync()}
        if root and st:
            out.update({
                "path": root, "label": volume_label(root),
                "device": {"device_id": st["device_id"], "clock": D.fmt_ts(st["clock"]) if st["clock"] is not None else "",
                           "records": st["records"], "last_card": st["last_card"],
                           "last_tap": D.fmt_ts(st["last_tap"]) if st["last_tap"] is not None else "",
                           "module": st["module"], "lecture": st["lecture"], "has_lecture": st["has_lecture"],
                           "since": D.fmt_ts(st["since"]) if st["since"] is not None else "",
                           "pending": st["pending"], "error": st["error"], "note": st["note"]},
                "cards": self.cards_state(st),
                "drift_seconds": (-self.pc_minus_device if self.pc_minus_device is not None else None)})
        return out

    def cards_state(self, st):
        """Does the device hold the same card list as the database? (None: this firmware does not say.)"""
        ids = self.db.card_ids()
        dev = st.get("cards")
        if dev is None:
            return {"device": None, "database": len(ids), "in_sync": None, "too_many": len(ids) > D.DEVICE_CARDS_MAX}
        same = dev == len(ids) and (dev == 0 or st.get("cards_crc") in (None, D.cards_crc(ids)))
        return {"device": dev, "database": len(ids), "in_sync": same, "too_many": len(ids) > D.DEVICE_CARDS_MAX}

    def write_settings(self, text):
        root = self.device_dir()
        if not root:
            raise ApiError(409, "The device is not connected. Plug it in with the USB-C cable.")
        data = text.encode("utf-8")
        if len(data) > MAX_SETTINGS_BYTES or b"\x00" in data:
            raise ApiError(413, "That settings file is too large for the device.")
        # Written in place, not via a temporary file and a rename: the device
        # is a tiny FAT volume and this keeps its directory tidy.
        try:
            with open(os.path.join(root, SETTINGS_NAME), "wb") as f:
                f.write(data)
                f.flush()
                os.fsync(f.fileno())
        except OSError as e:
            raise ApiError(500, "Could not write to the device: %s" % e)

    def device_offset(self):
        """Seconds to add to this PC's clock to get the device's (0 when unknown)."""
        return -self.pc_minus_device if self.pc_minus_device is not None else 0

    # ---- what the screens ask of the device (the web routes and the desktop window both call these)
    def import_everything(self):
        """
        Read every tap off the device into the database, now. True only when ATTEND.CSV held exactly
        the number of rows STATUS.TXT promises and none was unreadable: then the device may be cleared.
        """
        with self.lock:
            root = self.device_dir()
            if not root:
                return False
            st = D.parse_status(read_text(os.path.join(root, STATUS_NAME), 1024))
            rows, skipped = D.parse_attend(read_text(os.path.join(root, ATTEND_NAME)))
            if not st["ok"] or skipped or len(rows) != st["records"]:
                return False
            self._sync(root, st)
            self.sig = None                     # read STATUS.TXT afresh on the next poll
            return True

    def eject(self):
        """Eject the drive so the device applies SETTINGS.CSV and starts scanning. True when it worked."""
        with self.lock:
            root = self.device_dir()
            ok = bool(root) and not self.forced_dir and eject_drive(root)
            if ok:
                self.last_path = None
                self.sig = None
            return ok

    def start_lecture(self, module, title, sync_clock=True, clear_device=True):
        """
        Tell the device which lecture this is, and record it here. With @clear_device the device's
        records are imported first and the device deletes them when it starts the lecture. The drive
        is then ejected so the lecture starts at once. Returns {lecture, clock_set, cleared, ejected}.
        """
        module = D.clean_device_text(module, D.MODULE_BYTES)
        title = D.clean_device_text(title, D.LECTURE_BYTES)
        if not module:
            raise ApiError(400, "Choose or enter a module")
        if not title:
            raise ApiError(400, "Enter a lecture name")
        sync = sync_clock is not False
        st = self.status or {}
        now = D.now_ts()
        clear = bool(clear_device) and self.import_everything()
        text = D.build_settings(now=now if sync else None, module=module, lecture=title, new_session=True,
                                device_id=st.get("device_id", 0), cards=self.db.card_ids(), clear_log=clear)
        self.write_settings(text)                # first: if the device cannot be written, nothing is recorded here
        start = now if sync else now + int(self.device_offset())
        lecture = self.db.start_lecture(module, title, start)
        return {"lecture": lecture, "clock_set": sync, "cleared": clear, "ejected": self.eject()}

    def set_clock(self):
        """Set the device clock to this computer's time (and refresh its card list)."""
        st = self.status or {}
        self.write_settings(D.build_settings(now=D.now_ts(), device_id=st.get("device_id", 0), cards=self.db.card_ids()))

    def send_cards(self):
        """Give the device the card numbers of every registered student, so it can show green or red.
        Returns how many were sent."""
        st = self.status or {}
        ids = self.db.card_ids()
        self.write_settings(D.build_settings(device_id=st.get("device_id", 0), cards=ids))
        return len(ids)

    def set_device_id(self, value):
        dev = _int(value, "device number")
        if dev < 0 or dev >= 0xFFFFFFFF:
            raise ApiError(400, "The device number must be between 0 and 4294967294")
        self.write_settings(D.build_settings(device_id=dev))


def rotate_backup(db, data_dir):
    """Keep a dated copy of the database, one a day, the last BACKUPS_KEPT of them."""
    if not data_dir:
        return None
    folder = os.path.join(data_dir, "backups")
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, "attendance-%s.db" % datetime.date.today().strftime("%Y%m%d"))
    if not os.path.exists(path) and db.counts()["taps"] + db.counts()["students"] > 0:
        with open(path, "wb") as f:
            f.write(db.backup_bytes())
    files = sorted(f for f in os.listdir(folder) if re.fullmatch(r"attendance-\d{8}\.db", f))
    for old in files[:-BACKUPS_KEPT]:
        try:
            os.remove(os.path.join(folder, old))
        except OSError:
            pass
    return path


# --------------------------------------------------------------------------
# Actions and exports shared by the web routes and the desktop window
# --------------------------------------------------------------------------

CSV_TYPE = "text/csv; charset=utf-8"
HTML_TYPE = "text/html; charset=utf-8"
PDF_TYPE = "application/pdf"


def _int(v, what="number"):
    try:
        return int(v)
    except (TypeError, ValueError):
        raise ApiError(400, "That is not a valid %s" % what)


def opt_ts(v):
    """A typed date or date and time as a naive epoch; empty means None."""
    v = (v or "").strip() if isinstance(v, str) else v
    return D.parse_ts(v) if v else None


def _slug(s):
    return re.sub(r"[^A-Za-z0-9._-]+", "_", str(s)).strip("_") or "attendance"


def import_taps(db, text):
    """Taps from a saved ATTEND.CSV, when the device itself is not to hand. Returns {read, new, skipped}."""
    if not isinstance(text, str):
        raise ApiError(400, "expected {\"text\": \"...\"}")
    rows, skipped = D.parse_attend(text)
    if not rows:
        raise ApiError(400, "No taps found in that file. It should be the ATTEND.CSV from the device.")
    return {"read": len(rows), "new": db.add_taps(rows, 0), "skipped": skipped}


def edit_lecture(db, lecture_id, fields):
    """Change a lecture. @fields holds any of module, title, start, end (dates as typed; an empty end means running)."""
    kw = {}
    if "module" in fields:
        kw["module"] = fields["module"]
    if "title" in fields:
        kw["title"] = fields["title"]
    if "start" in fields:
        kw["start_ts"] = opt_ts(fields["start"])
    if "end" in fields:
        kw["end_ts"] = opt_ts(fields["end"])
    return db.update_lecture(lecture_id, **kw)


def save_module(db, old, code, title="", department=""):
    """Add a module, or edit one (renaming it first when @old differs from @code), as the Modules screen does."""
    if old and old != str(code or "").strip():
        db.rename_module(old, code)
    return db.upsert_module(code, title, department)


def _pdf_bytes(page):
    try:
        return R.render_pdf(page)
    except RuntimeError as e:
        raise ApiError(501, str(e))


def _export(kind, csv_text, page, base):
    """(data, content type, file name) for kind csv, html or pdf. @page is called only when needed."""
    if kind == "csv":
        return ("﻿" + csv_text()).encode("utf-8"), CSV_TYPE, base + ".csv"
    if kind == "html":
        return page().encode("utf-8"), HTML_TYPE, base + ".html"
    if kind == "pdf":
        return _pdf_bytes(page()), PDF_TYPE, base + ".pdf"
    raise ValueError(kind)


def lecture_export(db, lecture_id, kind):
    att = db.lecture_attendance(lecture_id)
    l = att["lecture"]
    return _export(kind, lambda: D.Database.lecture_csv(att), lambda: R.lecture_page(att),
                   "%s_%s_%s" % (_slug(l["date"]), _slug(l["module_code"]), _slug(l["title"])))


def module_export(db, code, kind, date_from=None, date_to=None):
    rep = db.module_report(code, date_from or None, date_to or None)
    return _export(kind, lambda: D.Database.module_csv(rep), lambda: R.module_page(rep), "%s_attendance" % _slug(code))


def student_export(db, card_id, kind):
    rep = db.student_report(card_id)
    return _export(kind, lambda: D.Database.student_csv(rep), lambda: R.student_page(rep),
                   "%s_attendance" % _slug(rep["student"]["name"]))


def students_export(db):
    return ("﻿" + db.students_csv()).encode("utf-8"), CSV_TYPE, "students.csv"


def taps_export(db, ts_from=None, ts_to=None):
    return ("﻿" + db.taps_csv(ts_from, ts_to)).encode("utf-8"), CSV_TYPE, "taps.csv"


def backup_export(db):
    return db.backup_bytes(), "application/octet-stream", "attendance-%s.db" % time.strftime("%Y%m%d-%H%M")


# --------------------------------------------------------------------------
# Demo data: a believable department, and a device that holds its taps
# --------------------------------------------------------------------------

def make_demo(data_dir, device_dir):
    """Fill @data_dir with a database and @device_dir with the device's three files."""
    import random
    rnd = random.Random(11)
    db = D.Database(os.path.join(data_dir, "attendance.db"))
    first = ["Amal", "Nimali", "Kasun", "Dilani", "Ruwan", "Sanduni", "Tharindu", "Ishara", "Chamara", "Hasini",
             "Pradeep", "Mihiri", "Lahiru", "Nadeesha", "Gayan", "Oshadi", "Supun", "Kavindi", "Dinesh", "Yashodha",
             "Isuru", "Sachini", "Buddhika", "Anushka", "Malith", "Thilini", "Janaka", "Piumi", "Harsha", "Dulani"]
    last = ["Perera", "Silva", "Fernando", "Jayasinghe", "Bandara", "Wickramasinghe", "Gunawardena", "Rajapaksa"]
    db.upsert_module("EN2090", "Circuits and Systems", "Electrical Engineering")
    db.upsert_module("MA1010", "Calculus", "Mathematics")
    db.upsert_module("CS1010", "Programming Fundamentals", "Computer Science")
    people = []
    for i, f in enumerate(first):
        card = 1000 + 7 * i
        dept = "Electrical Engineering" if i % 3 != 2 else "Computer Science"
        mods = ["MA1010"] + (["EN2090"] if dept.startswith("Elec") else ["CS1010"])
        db.upsert_student(card, "%s %s" % (f, last[i % len(last)]), "EN/%02d/%03d" % (20 + i % 3, 100 + i), dept, mods)
        people.append((card, mods))
    # Two cards nobody has registered yet: the "enrol me" demo.
    new_cards = [424242, 535353]

    today = datetime.date.today()
    day = lambda n: datetime.datetime.combine(today - datetime.timedelta(days=n), datetime.time(9, 0, 0))
    plan = [("EN2090", "Circuits Lecture 1", 21, 0.93), ("EN2090", "Circuits Lecture 2", 14, 0.85),
            ("EN2090", "Circuits Lecture 3", 7, 0.74), ("MA1010", "Calculus Lecture 1", 20, 0.9),
            ("MA1010", "Calculus Lecture 2", 13, 0.8), ("CS1010", "Intro to Python", 15, 0.88)]
    taps = []
    for module, title, ago, rate in plan:
        start = day(ago)
        lec = db.create_lecture(module, title, D.to_ts(start), D.to_ts(start) + 3600)
        for card, mods in people:
            if module in mods and rnd.random() < rate:
                taps.append((D.to_ts(start) + rnd.randint(2, 480), card))
    # One lecture today, still running, with a new card in it.
    now = datetime.datetime.now().replace(microsecond=0)
    start = now - datetime.timedelta(minutes=40)
    cur = db.start_lecture("EN2090", "Circuits Lecture 4", D.to_ts(start))
    for card, mods in people:
        if "EN2090" in mods and rnd.random() < 0.7:
            taps.append((D.to_ts(start) + rnd.randint(30, 1500), card))
    taps.append((D.to_ts(start) + 900, new_cards[0]))
    taps.append((D.to_ts(start) + 1200, new_cards[1]))
    # Some taps with no lecture running: attendance taken without starting one.
    loose = datetime.datetime.combine(today - datetime.timedelta(days=3), datetime.time(15, 30, 0))
    for card, mods in people[:9]:
        taps.append((D.to_ts(loose) + rnd.randint(0, 300), card))
    taps.sort()
    # The database starts with no taps; the device log holds them all, and the first read brings them in.
    db.close()
    del cur

    def pad(s, n):
        return s.ljust(n)[:n]

    attend = pad("DATE,TIME,CARD_ID", 30) + "\r\n"
    for ts, card in taps:
        attend += "%s,%s,%010d\r\n" % (D.fmt_ts(ts)[:10], D.fmt_ts(ts)[11:], card)
    last_ts, last_card = taps[-1]
    # The device already knows the registered students, but not the two new cards.
    cards_line = "%d registered (CRC %08X)" % (len(people), D.cards_crc([c for c, _ in people]))
    status = ("ATTENDANCE LOGGER\r\nDevice ID    : 0012648430\r\nClock        : %s\r\nAttendance   : %d records in ATTEND.CSV\r\n"
              "Cards        : %s\r\n"
              "Last tap     : %010d at %s\r\nLecture      : EN2090 / Circuits Lecture 4 (since %s)\r\nSETTINGS.CSV : unchanged\r\n"
              % (now.strftime("%Y-%m-%d %H:%M:%S"), len(taps), cards_line, last_card, D.fmt_ts(last_ts), start.strftime("%Y-%m-%d %H:%M:%S")))
    status = status.ljust(510) + "\r\n"
    settings = ("# Edit these lines, then eject the drive (or press the button). Add #NEWSESSION,1 to start another lecture with the same names.\r\n"
                "#TIME,%s\r\n#MODULE,EN2090\r\n#LECTURE,Circuits Lecture 4\r\n#DEVICE,0012648430\r\n" % now.strftime("%Y-%m-%d %H:%M:%S"))
    for name, text in ((ATTEND_NAME, attend), (STATUS_NAME, status), (SETTINGS_NAME, settings)):
        with open(os.path.join(device_dir, name), "w", newline="", encoding="utf-8") as f:
            f.write(text)


# --------------------------------------------------------------------------
# The web server
# --------------------------------------------------------------------------

def _download(data, ctype, filename=None):
    return ("download", data, ctype, filename)


def _file(export):
    """An export as a download; a page to print is shown, not saved."""
    data, ctype, filename = export
    return _download(data, ctype, None if ctype == HTML_TYPE else filename)


class Handler(BaseHTTPRequestHandler):
    server_version = "AttendanceCompanion/2.0"
    app = None          # set on the class by serve()
    routes = []

    def log_message(self, fmt, *args):      # keep the console quiet
        pass

    # ---- plumbing --------------------------------------------------------
    def _send(self, code, body, ctype="application/json; charset=utf-8", extra=None):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _json(self, code, obj):
        self._send(code, json.dumps(obj))

    def _host_ok(self):
        """Refuse requests meant for another site (DNS rebinding) or sent by one."""
        host = (self.headers.get("Host") or "").lower()
        port = self.server.server_address[1]
        if host not in ("127.0.0.1:%d" % port, "localhost:%d" % port):
            return False
        origin = self.headers.get("Origin")
        if origin and origin.lower() not in ("http://127.0.0.1:%d" % port, "http://localhost:%d" % port):
            return False
        return True

    def _read_body(self):
        """The request body, always consumed: answering without reading it makes
        some systems reset the connection, and the client never sees the answer."""
        try:
            length = int(self.headers.get("Content-Length") or "0")
        except ValueError:
            length = -1
        if length < 0:
            return None, -1
        data = self.rfile.read(min(length, MAX_BODY))
        remaining = length - len(data)
        while remaining > 0:                      # discard an oversize body without keeping it
            chunk = self.rfile.read(min(remaining, 65536))
            if not chunk:
                break
            remaining -= len(chunk)
        return data, length

    def _handle(self, method):
        raw, length = self._read_body()
        if not self._host_ok():
            return self._json(403, {"error": "forbidden"})
        if method != "GET" and self.headers.get("X-Attendance") != "1":
            return self._json(403, {"error": "forbidden"})
        parsed = urlparse(self.path)
        path = unquote(parsed.path)
        query = {k: v[0] for k, v in parse_qs(parsed.query).items()}

        if method == "GET" and not path.startswith("/api/"):
            return self._static(path)

        body = {}
        if method != "GET" and raw:
            if length > MAX_BODY:
                return self._json(413, {"error": "That is too large to send."})
            try:
                body = json.loads(raw.decode("utf-8"))
                if not isinstance(body, dict):
                    raise ValueError
            except (ValueError, UnicodeDecodeError):
                return self._json(400, {"error": "expected a JSON object"})

        for m, rx, fn in self.routes:
            if m != method:
                continue
            mt = rx.fullmatch(path)
            if mt:
                try:
                    result = fn(self.app, mt, query, body)
                except D.DbError as e:
                    return self._json(400, {"error": str(e)})
                except ApiError as e:
                    return self._json(e.code, {"error": e.message})
                if isinstance(result, tuple) and result and result[0] == "download":
                    _, data, ctype, filename = result
                    extra = {"Content-Disposition": 'attachment; filename="%s"' % filename} if filename else None
                    return self._send(200, data, ctype, extra)
                return self._json(200, result)
        return self._json(404, {"error": "not found"})

    def do_GET(self):
        self._handle("GET")

    def do_POST(self):
        self._handle("POST")

    def do_PATCH(self):
        self._handle("PATCH")

    def do_DELETE(self):
        self._handle("DELETE")

    def _static(self, path):
        if path in ("", "/"):
            path = "/index.html"
        rel = os.path.normpath(path.lstrip("/"))
        full = os.path.join(WEB, rel)
        if not os.path.abspath(full).startswith(os.path.abspath(WEB) + os.sep) or not os.path.isfile(full):
            return self._json(404, {"error": "not found"})
        ctype = mimetypes.guess_type(full)[0] or "application/octet-stream"
        if ctype.startswith("text/") or ctype in ("application/javascript", "application/json"):
            ctype += "; charset=utf-8"
        with open(full, "rb") as f:
            self._send(200, f.read(), ctype)


# ---- routes ---------------------------------------------------------------

def route(method, pattern):
    rx = re.compile(pattern)

    def deco(fn):
        Handler.routes.append((method, rx, fn))
        return fn
    return deco


@route("GET", r"/api/ping")
def r_ping(app, m, q, b):
    return {"app": "attendance-companion"}


@route("GET", r"/api/state")
def r_state(app, m, q, b):
    return app.refresh()


@route("POST", r"/api/sync")
def r_sync(app, m, q, b):
    state = app.refresh(force=True)
    if not state["connected"]:
        raise ApiError(409, "The device is not connected.")
    return state


@route("POST", r"/api/quit")
def r_quit(app, m, q, b):
    threading.Thread(target=app.server.shutdown, daemon=True).start()
    return {"ok": True}


# -- students
@route("GET", r"/api/students")
def r_students(app, m, q, b):
    return {"students": app.db.list_students(q.get("q"), q.get("module") or None, q.get("department") or None),
            "departments": app.db.departments()}


@route("POST", r"/api/students")
def r_student_save(app, m, q, b):
    return app.db.upsert_student(b.get("card_id"), b.get("name"), b.get("student_no", ""), b.get("department", ""),
                                 b.get("modules") if isinstance(b.get("modules"), list) else None)


@route("DELETE", r"/api/students/(\d+)")
def r_student_delete(app, m, q, b):
    app.db.delete_student(int(m.group(1)))
    return {"ok": True}


@route("POST", r"/api/students/import")
def r_student_import(app, m, q, b):
    text = b.get("text")
    if not isinstance(text, str):
        raise ApiError(400, "expected {\"text\": \"...\"}")
    return app.db.import_students(text)


@route("GET", r"/api/students\.csv")
def r_students_csv(app, m, q, b):
    return _file(students_export(app.db))


@route("GET", r"/api/cards/unregistered")
def r_unregistered(app, m, q, b):
    return {"cards": app.db.unregistered_cards()}


# -- modules
@route("GET", r"/api/modules")
def r_modules(app, m, q, b):
    return {"modules": app.db.list_modules(), "departments": app.db.departments()}


@route("POST", r"/api/modules")
def r_module_save(app, m, q, b):
    return app.db.upsert_module(b.get("code"), b.get("title", ""), b.get("department", ""))


@route("POST", r"/api/modules/rename")
def r_module_rename(app, m, q, b):
    app.db.rename_module(b.get("old", ""), b.get("new", ""))
    return {"ok": True}


@route("DELETE", r"/api/modules/(.+)")
def r_module_delete(app, m, q, b):
    app.db.delete_module(m.group(1), force=q.get("force") == "1")
    return {"ok": True}


@route("POST", r"/api/modules/(.+)/enroll")
def r_module_enroll(app, m, q, b):
    app.db.set_enrollment(m.group(1), b.get("card_ids") or [], b.get("mode", "add"))
    return {"ok": True}


# -- lectures
@route("GET", r"/api/lectures")
def r_lectures(app, m, q, b):
    return {"lectures": app.db.list_lectures(q.get("module") or None, q.get("from") or None, q.get("to") or None),
            "unassigned": app.db.unassigned_days()}


@route("POST", r"/api/lectures/start")
def r_lecture_start(app, m, q, b):
    """Tell the device which lecture this is, and record it here."""
    return app.start_lecture(b.get("module"), b.get("title"), b.get("sync_clock", True))


@route("POST", r"/api/lectures")
def r_lecture_create(app, m, q, b):
    return app.db.create_lecture(b.get("module"), b.get("title"), opt_ts(b.get("start")), opt_ts(b.get("end")))


@route("PATCH", r"/api/lectures/(\d+)")
def r_lecture_update(app, m, q, b):
    return edit_lecture(app.db, int(m.group(1)), b)


@route("POST", r"/api/lectures/(\d+)/end")
def r_lecture_end(app, m, q, b):
    return app.db.end_lecture(int(m.group(1)))


@route("DELETE", r"/api/lectures/(\d+)")
def r_lecture_delete(app, m, q, b):
    app.db.delete_lecture(int(m.group(1)))
    return {"ok": True}


@route("GET", r"/api/lectures/(\d+)")
def r_lecture(app, m, q, b):
    return app.db.lecture_attendance(int(m.group(1)))


@route("GET", r"/api/lectures/(\d+)\.csv")
def r_lecture_csv(app, m, q, b):
    return _file(lecture_export(app.db, int(m.group(1)), "csv"))


@route("GET", r"/api/lectures/(\d+)\.html")
def r_lecture_html(app, m, q, b):
    return _file(lecture_export(app.db, int(m.group(1)), "html"))


@route("GET", r"/api/lectures/(\d+)\.pdf")
def r_lecture_pdf(app, m, q, b):
    return _file(lecture_export(app.db, int(m.group(1)), "pdf"))


@route("POST", r"/api/unassigned/lecture")
def r_unassigned_lecture(app, m, q, b):
    return app.db.lecture_from_unassigned(b.get("date", ""), b.get("module"), b.get("title"))


# -- reports
@route("GET", r"/api/reports/module/(.+?)(\.csv|\.html|\.pdf)?")
def r_report_module(app, m, q, b):
    code, ext = m.group(1), m.group(2)
    if ext:
        return _file(module_export(app.db, code, ext[1:], q.get("from"), q.get("to")))
    return app.db.module_report(code, q.get("from") or None, q.get("to") or None)


@route("GET", r"/api/reports/student/(\d+)(\.csv|\.html|\.pdf)?")
def r_report_student(app, m, q, b):
    card, ext = int(m.group(1)), m.group(2)
    if ext:
        return _file(student_export(app.db, card, ext[1:]))
    return app.db.student_report(card)


# -- raw taps and the device
@route("GET", r"/api/taps")
def r_taps(app, m, q, b):
    card = D.parse_card(q["card"]) if q.get("card") else None
    return {"taps": app.db.list_taps(opt_ts(q.get("from")), opt_ts(q.get("to")), card, limit=min(_int(q.get("limit", 500)), 20000))}


@route("GET", r"/api/taps\.csv")
def r_taps_csv(app, m, q, b):
    return _file(taps_export(app.db, opt_ts(q.get("from")), opt_ts(q.get("to"))))


@route("POST", r"/api/taps/import")
def r_taps_import(app, m, q, b):
    """Taps from a saved ATTEND.CSV, when the device itself is not to hand."""
    return import_taps(app.db, b.get("text"))


@route("POST", r"/api/device/clock")
def r_device_clock(app, m, q, b):
    app.set_clock()
    return {"ok": True}


@route("POST", r"/api/device/cards")
def r_device_cards(app, m, q, b):
    """Give the device the card numbers of every registered student, so it can show green or red."""
    return {"ok": True, "count": app.send_cards()}


@route("POST", r"/api/device/id")
def r_device_id(app, m, q, b):
    app.set_device_id(b.get("device_id"))
    return {"ok": True}


@route("GET", r"/api/backup\.db")
def r_backup(app, m, q, b):
    return _file(backup_export(app.db))


# --------------------------------------------------------------------------
# Starting up
# --------------------------------------------------------------------------

def serve(port, app, open_browser):
    Handler.app = app
    server = None
    for p in range(port, port + 20):
        try:
            server = ThreadingHTTPServer(("127.0.0.1", p), Handler)
            break
        except OSError:
            continue
    if server is None:
        sys.exit("Could not find a free port near %d." % port)
    app.server = server
    url = "http://127.0.0.1:%d/" % server.server_address[1]
    print("Attendance app running at %s" % url, flush=True)
    print("Close this window, or press Ctrl+C, to stop it.", flush=True)
    if open_browser:
        threading.Timer(0.4, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


def already_running(port):
    """If another copy is serving, just open it instead of starting a second."""
    import urllib.request
    try:
        with urllib.request.urlopen("http://127.0.0.1:%d/api/ping" % port, timeout=0.6) as r:
            return json.loads(r.read().decode("utf-8")).get("app") == "attendance-companion"
    except Exception:
        return False


def default_data_dir():
    return os.path.join(os.path.expanduser("~"), "AttendanceLogger")


def main(argv=None):
    ap = argparse.ArgumentParser(description="Attendance Logger companion app")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--no-browser", action="store_true", help="do not open a browser window")
    ap.add_argument("--device-dir", help="use this folder as the device instead of searching drives")
    ap.add_argument("--data-dir", help="where the database lives (default: %s)" % default_data_dir())
    ap.add_argument("--demo", action="store_true", help="use made-up data in temporary folders; touches nothing real")
    args = ap.parse_args(argv)

    forced, data_dir = args.device_dir, args.data_dir or default_data_dir()
    if args.demo:
        data_dir = tempfile.mkdtemp(prefix="attendance-demo-data-")
        forced = tempfile.mkdtemp(prefix="attendance-demo-device-")
        make_demo(data_dir, forced)
        print("Demo data in %s and %s (nothing real is touched)" % (data_dir, forced), flush=True)
    elif not forced and already_running(args.port):
        url = "http://127.0.0.1:%d/" % args.port
        print("Already running at %s" % url, flush=True)
        if not args.no_browser:
            webbrowser.open(url)
        return 0
    os.makedirs(data_dir, exist_ok=True)
    db = D.Database(os.path.join(data_dir, "attendance.db"))
    if not args.demo:
        try:
            rotate_backup(db, data_dir)
        except OSError:
            pass
    print("Database: %s" % db.path, flush=True)
    serve(args.port, App(db, forced, data_dir), not args.no_browser)
    return 0


if __name__ == "__main__":
    sys.exit(main())

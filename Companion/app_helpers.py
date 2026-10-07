"""
Attendance Logger companion app: what the window (attendance_qt.py) says and decides, with no GUI in it.

The wording and rules the window shows (the suggested lecture name, the banners, the device status, the
battery, the confirmations), a lecture's list as CSV or PDF, waiting for a tapped card or for the device to
come back, the one-window lock, the preferences file, and the worker thread that runs anything touching the
device. The device itself and the actions on it are attendance_app.py's; the database is attendance_db.py's.
"""
import csv
import io
import json
import os
import queue
import re
import threading
import time

import attendance_app as A
import attendance_db as D
import report_pages as R

APP_TITLE = "Attendance Logger"
POLL_MS = 2000
LOCK_NAME = "companion.lock"
PREFS_NAME = "companion-gui.json"
DRIFT_LIMIT = 120              # seconds before the clock is called wrong
DEFAULT_MODULE = "GENERAL"     # the device stores a module with every lecture; this window does not ask for one
TAP_POLL_MS = 500              # how often LASTCARD.TXT is read while waiting for a card to be tapped
TAP_INTRO = ("Tap the card on the device now. The device must be plugged in to this computer; its light flashes "
             "green when it has read the card.")
ERASE_WORD = "ERASE"           # typed to confirm erasing the device without saving its records
ERASE_TEXT = ("This erases every tap and lecture on the device WITHOUT copying them to this computer first. Records "
              "that were not read in here yet are lost for good. Use Clear device records instead, unless the "
              "device's records cannot be read.\n\nThe device keeps its number and clock, and numbers its lectures "
              "from 1 again. The students and everything already on this computer are kept.")
CLEAR_WORD = "CLEAR"           # typed to confirm clearing the records on this computer
CLEAR_TEXT = ("This deletes every lecture and every tap from this computer. The students, their cards and modules "
              "are kept. A copy of the database is saved first, in the backups folder next to it.\n\nRecords still on "
              "the device come back the next time it is read, unless the device is cleared too.")
CLEAR_DEVICE_TEXT = "Also clear the device's records (erased there without copying them here)"
CONNECT_POLL_MS = 1000         # how often Connect looks for the device's drive ...
CONNECT_WAIT_S = 30            # ... and for how long
CONNECT_TEXT = ("Plug the device's cable into this computer. If it is already plugged in and taking attendance, press "
                "the device's button twice quickly: its drive comes back in a few seconds.")
CONNECT_TIMEOUT = ("The device did not appear. Check the cable, press the device's button twice quickly, and press "
                   "Connect again.")
EJECTED_TEXT = "Ejected. The device is now taking attendance with the cable in."
EXPORTS = (                    # the whole-database lists: (what, kind, menu text)
    ("taps", "csv", "All attendance records (CSV)\u2026"), ("taps", "pdf", "All attendance records (PDF)\u2026"),
    ("students", "csv", "Student list (CSV)\u2026"), ("students", "pdf", "Student list (PDF)\u2026"),
)

PAL = {                        # the window's colours
    "bg": "#F4F6FC", "card": "#FFFFFF", "line": "#D7DBEC", "text": "#1E2235", "muted": "#5E6478",
    "stripe": "#F3F5FD", "select": "#C9D2F5", "toolbar": "#E6E9F8",
    "indigo": "#3F51B5", "indigo_dark": "#283593", "blue": "#1E88E5", "blue_dark": "#1565C0",
    "teal": "#00897B", "teal_dark": "#00695C", "green": "#43A047", "green_dark": "#2E7D32",
    "green_soft": "#E8F5E9", "red": "#E53935", "red_dark": "#C62828", "orange": "#FB8C00",
    "orange_dark": "#EF6C00", "amber_soft": "#FFF4E0", "amber_text": "#8A4B00", "purple": "#8E24AA",
    "purple_dark": "#6A1B9A",
}
COLORS = {                     # banner and status colours: background, text
    "ok": ("#E8F5E9", "#1B5E20"),
    "info": ("#E3F2FD", "#0D47A1"),
    "warn": ("#FFF4E0", "#7A4100"),
    "bad": ("#FDECEA", "#8A1F17"),
    "off": ("#ECEFF1", "#37474F"),
}


# --------------------------------------------------------------------------
# Plain helpers
# --------------------------------------------------------------------------

def plural(n, one, many=None):
    return "%d %s" % (n, one if n == 1 else (many or one + "s"))


def card10(n):
    return D.fmt_card(int(n))


def matches(qs, parts):
    """Case-insensitive substring search over @parts."""
    if not qs:
        return True
    qs = qs.lower()
    return any(qs in str("" if p is None else p).lower() for p in parts)


def suggest_title(lectures, module=None, sent=None):
    """The next lecture name: the newest lecture's (of @module, when given) number plus one."""
    last = ""
    for l in lectures:                       # newest first
        if not module or l["module_code"] == module:
            last = l["title"]
            break
    if sent and (not module or sent.get("module") == module):
        last = sent["title"]
    m = re.match(r"^(.*?)(\d+)\s*$", last)
    if m:
        return m.group(1) + str(int(m.group(2)) + 1)
    return last + " 2" if last else "Lecture 1"


def lecture_module(lectures):
    """The module a lecture started here is filed under: the newest lecture's, so the device's own
    lectures (a long press keeps the module) stay together; GENERAL when there is none yet."""
    for l in lectures:                       # newest first
        if l["module_code"] and l["module_code"] != D.UNASSIGNED_MODULE:
            return l["module_code"]
    return DEFAULT_MODULE


def present_students(detail):
    """Everyone registered who tapped in a lecture, in the order they came."""
    if not detail:
        return []
    return sorted(detail["present"] + detail["present_other"], key=lambda e: (e["ts"], e["card_id"]))


def lecture_table(detail):
    """A lecture's attendance as (headers, rows): index number, name, card, time; unregistered cards last."""
    rows = [[s["student_no"], s["name"], card10(s["card_id"]), s["time"]] for s in present_students(detail)]
    rows += [["", "(card not registered)", card10(u["card_id"]), u["time"]] for u in detail["unregistered"]]
    return ["Index No", "Name", "Card", "Time"], rows


def lecture_csv(detail):
    """A lecture's attendance as CSV text (lecture_table())."""
    out = io.StringIO()
    w = csv.writer(out, lineterminator="\r\n")
    head, rows = lecture_table(detail)
    w.writerow(head)
    w.writerows(rows)
    return out.getvalue()


def lecture_file(detail, kind):
    """(data, file name) of a lecture's list as kind csv or pdf, as Save list saves it."""
    l = detail["lecture"]
    base = "attendance-%s-%s" % (A._slug(l["title"]), l["date"])
    if kind == "pdf":
        head, rows = lecture_table(detail)
        sub = "%s, %s to %s  \u00b7  %d present" % (nice_date(l["start_ts"]), l["start_text"][11:16],
                                                    l["end_text"][11:16] if not l["running"] else "now",
                                                    len(present_students(detail)))
        if detail["unregistered"]:
            sub += ", %s not registered" % plural(len(detail["unregistered"]), "card")
        return R.table_pdf(l["title"], sub, head, rows), base + ".pdf"
    return lecture_csv(detail).encode("utf-8-sig"), base + ".csv"     # the BOM lets Excel read the names right


def export_file(db, what, kind):
    """(data, file name) of a whole-database list (EXPORTS): every tap, or the students; kind csv or pdf."""
    data, _, name = (A.taps_export(db, kind=kind) if what == "taps" else A.students_export(db, kind=kind))
    return data, name


def eject_failed_text(why):
    """What to say when an eject did not work, with App.last_eject_error's reason."""
    return "Could not eject. " + (why or "Use Eject in the file manager, or press the button on the device once.")


def not_ejected_note(r):
    """" (not ejected: why)" for an action's result {ejected, eject_error} whose eject failed; "" otherwise."""
    if r.get("ejected") or not r.get("eject_error"):
        return ""
    return " (Not ejected: %s)" % r["eject_error"].rstrip(".")


def clear_done_text(r):
    """What to say once the records on this computer were cleared (App.clear_records()'s result)."""
    text = "Deleted %s and %s from this computer. A copy of the database from before is at:\n%s" % (
        plural(r["lectures"], "lecture"), plural(r["taps"], "tap"), r["backup"])
    if r.get("device_error"):
        text += "\n\nThe device was not cleared: %s" % r["device_error"]
    elif r.get("device") is not None:
        text += ("\n\nThe device's records were erased too." if r["device"].get("ejected") else
                 "\n\nThe device was told to erase its records: eject the drive or press its button to do it now."
                 + not_ejected_note(r["device"]))
    else:
        text += "\n\nRecords still on the device come back the next time it is read."
    return text


def nice_date(ts):
    return D.from_ts(ts).strftime("%a %d %b %Y")


def drift_minutes(drift):
    return int(abs(drift) / 60.0 + 0.5)


def lecture_clock_text(st):
    drift = st.get("drift_seconds") if st and st.get("connected") else None
    if drift is None:
        return ""
    if abs(drift) <= DRIFT_LIMIT:
        return "The device clock is right."
    return "The device clock is %d minutes %s." % (drift_minutes(drift), "ahead" if drift > 0 else "behind")


def device_summary(st, demo=False):
    """(kind, title, detail) for the device status in the header."""
    if st and st.get("connected"):
        d = st.get("device") or {}
        bits = [plural(d.get("records", 0), "tap") + " on the device"]
        drift = st.get("drift_seconds")
        if drift is not None and abs(drift) > DRIFT_LIMIT:
            bits.append("clock %s %d min" % ("ahead" if drift > 0 else "behind", drift_minutes(drift)))
        return "ok", ("Demo device" if demo else "Device connected"), " \u00b7 ".join(bits)
    if st is None:
        return "off", "Looking for the device\u2026", ""
    return "wait", "Device not connected", "plug it in, or press Connect"


def battery_level(st):
    """(percent, mV or None, colour) for the battery of a connected device, the colour a PAL name: green from
    50 %, orange from 20 %, red below. None with no device, before its first reading (STATUS.TXT says
    unknown), or with firmware that does not say."""
    d = st.get("device") if st and st.get("connected") else None
    pct = d.get("battery_percent") if d else None
    if pct is None:
        return None
    pct = max(0, min(100, int(pct)))
    return pct, d.get("battery_mv"), ("green" if pct >= 50 else ("orange" if pct >= 20 else "red"))


def battery_tip(level):
    """The note shown over the battery icon."""
    pct, mv, _ = level
    return ("Battery %d %%%s. " % (pct, " (%d mV)" % mv if mv else "") +
            "While it is plugged in the cell is charging, so this reads a little high.")


class ConnectWait:
    """
    Connect, waiting for the device's drive to come back (a replug, or a quick double press of its button
    while the cable is in): looked for every CONNECT_POLL_MS for CONNECT_WAIT_S. The PC cannot bring the
    drive back by itself; this only watches for it.
    """

    def __init__(self, seconds=CONNECT_WAIT_S, clock=time.monotonic):
        self.clock = clock
        self.until = clock() + seconds

    def left(self):
        """Whole seconds still to wait."""
        return max(0, int(self.until - self.clock() + 0.999))

    def expired(self):
        return self.clock() >= self.until

    def text(self):
        return "Looking for the device\u2026 (%d s)" % self.left()


class TapWatch:
    """
    Waiting for a card to be tapped on the plugged-in device: LASTCARD.TXT's Taps counter going up. Fed each
    App.last_card() reading (None: no device, or firmware without the file); feed() returns the card number
    once a new tap arrives. The counter starts at 0 at every plug-in, so after a reading with no device any tap
    counts, and a counter that went down means the device was plugged in again.
    """

    def __init__(self):
        self.base = None
        self.absent = False

    def feed(self, lc):
        if lc is None:
            self.base, self.absent = None, True
            return None
        taps = lc["taps"]
        if self.base is None:
            self.base = 0 if self.absent else taps
        elif taps < self.base:
            self.base = 0
        if taps > self.base:
            self.base = taps
            return lc["card_id"]
        return None


def tap_wait_text(lc, connected):
    """What the tap dialog says while it waits."""
    if lc is not None:
        return "Waiting for a card\u2026"
    if connected:
        return ("The device does not report taps while it is plugged in. It needs the updated firmware; until then, "
                "tap the card with the device unplugged, then plug it in and pick the card from the list.")
    return "Plug the device in with the USB-C cable. (It needs the updated firmware.)"


def tap_owner_text(card_id, owner):
    """The card tapped already belongs to @owner (a student)."""
    who = owner["name"] + (" (%s)" % owner["student_no"] if owner.get("student_no") else "")
    return "Card %s already belongs to %s. Tap another card, or Cancel." % (card10(card_id), who)


def banner_list(st, sent, tab=None, dismissed=()):
    """The banners under the toolbar. Each is {key, kind, title, text, action}; a banner with an action
    ('eject') carries a button and cannot be dismissed."""
    out = []

    def add(key, kind, title, text, action=None):
        if action is None and key in dismissed:
            return
        out.append({"key": key, "kind": kind, "title": title, "text": text, "action": action})

    if st and st.get("connected") and st.get("device"):
        d = st["device"]
        if d.get("error"):
            add("err:" + d["error"], "bad", "The device refused the last settings file",
                re.sub(r"^ERROR,?\s*", "", d["error"]))
        if d.get("pending"):
            add("pending", "info", "Changes are waiting on the device.",
                "Eject the drive, or press the button on the device, to apply them.", action="eject")
        drift = st.get("drift_seconds")
        if drift is not None and abs(drift) > DRIFT_LIMIT:
            add("drift", "warn", "The device clock is %d minutes %s this computer."
                % (drift_minutes(drift), "ahead of" if drift > 0 else "behind"), "Press Set device time to fix it.")
    if sent and sent.get("done"):
        add("applied:" + sent["title"], "ok", "Lecture started on the device \u2713",
            "%s. Students can tap their cards now." % sent["title"])
    elif sent and sent.get("ejected"):
        add("await:" + sent["title"], "ok", "Sent to the device and ejected.",
            "%s. Green light and two buzzes on the device means the lecture started: students can tap their cards "
            "now, with the cable still in. Red light and three buzzes means it was refused. %s"
            % (sent["title"], "The device's old records are saved here and were deleted from it." if sent.get("cleared")
               else "The device kept its old records."))
    elif sent:
        add("await:" + sent["title"], "info", "Sent to the device.",
            "%sNow eject the drive, or press the button on the device once. The device then checks the file: green "
            "light and two buzzes means started, red light and three buzzes means it was refused."
            % ("It was not ejected: %s " % sent["eject_error"] if sent.get("eject_error") else ""), action="eject")
    return out


def err_text(e):
    """The message to show for a refused action."""
    if isinstance(e, A.ApiError):
        return e.message
    if isinstance(e, (D.DbError, OSError)):
        return str(e)
    return "Something went wrong: %s" % e


def load_prefs(data_dir):
    try:
        with open(os.path.join(data_dir, PREFS_NAME), "r", encoding="utf-8") as f:
            p = json.load(f)
        return p if isinstance(p, dict) else {}
    except (OSError, ValueError, TypeError):
        return {}


def save_prefs(data_dir, prefs):
    if not data_dir:
        return
    try:
        with open(os.path.join(data_dir, PREFS_NAME), "w", encoding="utf-8") as f:
            json.dump(prefs, f)
    except OSError:
        pass


class InstanceLock:
    """One window per data folder. The operating system drops the lock when the process ends, even in a crash."""

    def __init__(self, folder):
        self.path = os.path.join(folder, LOCK_NAME)
        self.f = None

    def acquire(self):
        f = open(self.path, "a+")
        try:
            if os.name == "nt":
                import msvcrt
                f.seek(0)
                msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            f.close()
            return False
        self.f = f
        return True

    def release(self):
        if not self.f:
            return
        try:
            if os.name == "nt":
                import msvcrt
                self.f.seek(0)
                msvcrt.locking(self.f.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl
                fcntl.flock(self.f.fileno(), fcntl.LOCK_UN)
        except OSError:
            pass
        self.f.close()
        self.f = None


class Worker:
    """One background thread that runs jobs in order. Each result, or the exception, goes back on
    @results as (callback, value) for the window's thread to call; this thread never touches a widget."""

    def __init__(self, results):
        self.jobs = queue.Queue()
        self.results = results
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def submit(self, fn, done=None, failed=None):
        self.jobs.put((fn, done, failed))

    def _run(self):
        while True:
            job = self.jobs.get()
            if job is None:
                return
            fn, done, failed = job
            try:
                value = fn()
            except Exception as e:           # handed to the window, which shows it
                self.results.put((failed, e))
            else:
                self.results.put((done, value))

    def stop(self, timeout=5.0):
        self.jobs.put(None)
        self.thread.join(timeout)

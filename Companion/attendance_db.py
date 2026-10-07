"""
The PC-side database: students, modules, enrolments, lectures and every tap.

The device records only a card number and a time. Everything else, who the card
belongs to, their department, the modules they take and the lectures that were
held, lives here, in one SQLite file, and attendance is worked out by matching
the two.

Times are whole seconds counted as if the device's local clock were UTC
("naive" epoch): the device keeps local time with no time zone, and treating it
the same way everywhere means nothing here can shift by an hour at a daylight
saving change or when the PC and the device sit in different zones.
"""
import bisect
import calendar
import csv
import datetime as _dt
import io
import re
import sqlite3
import struct
import threading
import zlib

SCHEMA_VERSION = 1
CARD_MAX = 0xFFFFFEFF          # card numbers from 0xFFFFFF00 up are reserved by the firmware
DEVICE_CARDS_MAX = 1000        # registered cards the device can hold
MODULE_BYTES = 24              # what the device can hold of a module code ...
LECTURE_BYTES = 32             # ... and of a lecture title
MAX_LECTURE_SECONDS = 6 * 3600  # a lecture with no end lasts at most this long (the device's own look-back)
UNASSIGNED_MODULE = "UNASSIGNED"  # where a lecture the device started with no module name is filed

_CTRL = re.compile(r"[\x00-\x1f\x7f]")


class DbError(ValueError):
    """A request the database refuses; the message is meant to be shown to the user."""


# --------------------------------------------------------------------------
# Times
# --------------------------------------------------------------------------

def to_ts(dt):
    """A naive datetime as a naive epoch."""
    return calendar.timegm(dt.timetuple())


def from_ts(ts):
    return _dt.datetime.fromtimestamp(ts, _dt.timezone.utc).replace(tzinfo=None)


def fmt_ts(ts):
    return from_ts(ts).strftime("%Y-%m-%d %H:%M:%S")


def fmt_date(ts):
    return from_ts(ts).strftime("%Y-%m-%d")


def parse_ts(text):
    """'YYYY-MM-DD[ T]HH:MM[:SS]' or a date alone (midnight). Returns a naive epoch."""
    t = str(text).strip()
    m = re.fullmatch(r"(\d{4})-(\d{1,2})-(\d{1,2})(?:[ T](\d{1,2}):(\d{2})(?::(\d{2}))?)?", t)
    if not m:
        raise DbError("Dates look like 2026-10-06 or 2026-10-06 09:30")
    y, mo, d, h, mi, s = (int(x) if x is not None else 0 for x in m.groups())
    try:
        return to_ts(_dt.datetime(y, mo, d, h, mi, s))
    except ValueError:
        raise DbError("That is not a real date: " + t)


def now_ts():
    """This computer's local time, as a naive epoch."""
    return to_ts(_dt.datetime.now())


# --------------------------------------------------------------------------
# Text
# --------------------------------------------------------------------------

def cut_utf8(s, max_bytes):
    """Cut to at most max_bytes of UTF-8 without splitting a character."""
    out, n = [], 0
    for ch in s:
        b = len(ch.encode("utf-8"))
        if n + b > max_bytes:
            break
        out.append(ch)
        n += b
    return "".join(out)


def clean_device_text(s, max_bytes):
    """Text as the device will store it: separators become spaces, cut to size."""
    t = _CTRL.sub(" ", str(s if s is not None else "")).replace(",", " ").replace('"', " ").strip()
    return cut_utf8(t, max_bytes).rstrip()


def clean_free_text(s, max_chars=80):
    t = _CTRL.sub(" ", str(s if s is not None else "")).strip()
    return re.sub(r"\s+", " ", t)[:max_chars].strip()


def parse_card(value):
    """A card number as typed: decimal (leading zeros fine) or 0x hex."""
    t = str(value if value is not None else "").strip()
    if not t:
        raise DbError("Enter the card number")
    if re.fullmatch(r"0[xX][0-9a-fA-F]+", t):
        v = int(t, 16)
    elif re.fullmatch(r"[0-9]+", t):
        v = int(t, 10)
    else:
        raise DbError("A card number is digits only, like 123456 (or 0x1E240): " + t)
    if v == 0:
        raise DbError("A card number cannot be 0")
    if v > CARD_MAX:
        raise DbError("That card number is too large: " + t)
    return v


def fmt_card(card_id):
    return "%010d" % card_id


# --------------------------------------------------------------------------
# ATTEND.CSV, LECTURES.CSV and STATUS.TXT as the device writes them
# --------------------------------------------------------------------------

_ATTEND_ROW = re.compile(r"^\s*(\d{4})-(\d\d)-(\d\d)\s*,\s*(\d\d):(\d\d):(\d\d)\s*,\s*(\d+)")


def parse_attend(text):
    """Rows of ATTEND.CSV as [(ts, card_id)] in file order, plus a count of lines skipped."""
    rows, skipped = [], 0
    for line in str(text).lstrip("\ufeff").replace("\x00", "").splitlines():
        if not line.strip() or line.lstrip().upper().startswith("DATE"):
            continue
        m = _ATTEND_ROW.match(line)
        if not m:
            skipped += 1
            continue
        y, mo, d, h, mi, s, card = (int(x) for x in m.groups())
        try:
            ts = to_ts(_dt.datetime(y, mo, d, h, mi, s))
        except ValueError:
            skipped += 1
            continue
        if card <= 0 or card > 0xFFFFFFFF:
            skipped += 1
            continue
        rows.append((ts, card))
    return rows, skipped


_LECTURE_ROW = re.compile(r"^(\d{4})-(\d\d)-(\d\d),(\d\d):(\d\d):(\d\d)$")


def parse_lectures(text):
    """
    Rows of LECTURES.CSV (every lecture start in the device's log) as [(ts, module, lecture)] in file
    order, plus a count of lines skipped. Rows are 128 bytes, space padded; the names never hold a comma,
    and the module is empty when the device started a lecture before any had been named. A NUL ends the data.
    """
    rows, skipped = [], 0
    for line in str(text).lstrip("﻿").split("\x00", 1)[0].splitlines():
        if not line.strip() or line.lstrip().upper().startswith("DATE"):
            continue
        parts = line.strip().split(",")
        m = _LECTURE_ROW.match(",".join(p.strip() for p in parts[:2])) if len(parts) == 4 else None
        if not m:
            skipped += 1
            continue
        try:
            ts = to_ts(_dt.datetime(*(int(x) for x in m.groups())))
        except ValueError:
            skipped += 1
            continue
        rows.append((ts, parts[2].strip(), parts[3].strip()))
    return rows, skipped


def parse_status(text):
    """STATUS.TXT as a dict. 'ok' is false for any other file."""
    raw = str(text or "").lstrip("\ufeff")
    out = {"ok": raw.lstrip().startswith("ATTENDANCE LOGGER"), "device_id": 0, "clock": None, "records": 0,
           "last_card": None, "last_tap": None, "module": "", "lecture": "", "since": None, "has_lecture": False,
           "pending": False, "error": "", "note": "", "cards": None, "cards_crc": None}
    for line in raw.splitlines():
        m = re.match(r"^([A-Za-z][A-Za-z.\s]*?)\s*:\s*(.*?)\s*$", line)
        if not m:
            continue
        key, val = m.group(1).strip().lower(), m.group(2)
        if key == "device id":
            out["device_id"] = int(val) if val.isdigit() else 0
        elif key == "clock":
            try:
                out["clock"] = parse_ts(val[:19])
            except DbError:
                pass
        elif key == "attendance":
            mm = re.match(r"(\d+)", val)
            out["records"] = int(mm.group(1)) if mm else 0
        elif key == "last tap":
            mm = re.match(r"(\d+) at (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)", val)
            if mm:
                out["last_card"] = int(mm.group(1))
                out["last_tap"] = parse_ts(mm.group(2))
        elif key == "lecture":
            if val and val != "none set":
                mm = re.match(r"^(.*?) ?/ (.*?)(?: \(since (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)\))?$", val)
                if mm:
                    out["module"], out["lecture"] = mm.group(1).strip(), mm.group(2).strip()
                    if mm.group(3):
                        out["since"] = parse_ts(mm.group(3))
                    out["has_lecture"] = True
        elif key == "cards":
            mm = re.match(r"(\d+) registered(?: \(CRC ([0-9A-Fa-f]{8})\))?", val)
            if mm:
                out["cards"] = int(mm.group(1))
                out["cards_crc"] = int(mm.group(2), 16) if mm.group(2) else None
            elif val.lower().startswith("none"):
                out["cards"], out["cards_crc"] = 0, 0
        elif key == "settings.csv":
            out["note"] = val
            if val.upper().startswith("ERROR"):
                out["error"] = val
            elif "will be applied" in val:
                out["pending"] = True
    return out


def cards_crc(card_ids):
    """The CRC-32 the device keeps for a card list: the numbers, ascending, as little-endian 32-bit words."""
    ids = sorted(set(card_ids))
    return zlib.crc32(b"".join(struct.pack("<I", i) for i in ids)) & 0xFFFFFFFF


def build_settings(now=None, module=None, lecture=None, new_session=False, device_id=0, echo_time=None, cards=None,
                   clear_log=False):
    """
    The SETTINGS.CSV to put on the device. now: a naive epoch to set the clock to (None leaves
    the clock alone); echo_time: the #TIME text the device showed, repeated so that the clock is not touched.
    module / lecture None leaves the line out; "" clears it.
    cards: the registered card numbers the device should compare taps with (None leaves its list alone,
    an empty list clears it). The device wants them ascending, without repeats, and at most DEVICE_CARDS_MAX.
    clear_log: the device deletes every record it holds (only once they are safely in this database).
    """
    lines = ['# Edit these lines, then eject the drive (or press the button). Add #NEWSESSION,1 to start another lecture with the same names.']
    if now is not None:
        lines.append("#TIME," + fmt_ts(now))
    elif echo_time:
        lines.append("#TIME," + echo_time)
    if module is not None:
        lines.append("#MODULE," + clean_device_text(module, MODULE_BYTES))
    if lecture is not None:
        lines.append("#LECTURE," + clean_device_text(lecture, LECTURE_BYTES))
    if new_session:
        lines.append("#NEWSESSION,1")
    if clear_log:
        lines.append("#CLEARLOG,1")
    if device_id:
        lines.append("#DEVICE,%010d" % device_id)
    if cards is not None:
        ids = sorted(set(int(c) for c in cards))
        if len(ids) > DEVICE_CARDS_MAX:
            raise DbError("The device can hold %d cards and there are %d students. Delete some, or archive "
                          "the ones who have left." % (DEVICE_CARDS_MAX, len(ids)))
        if ids and (ids[0] < 1 or ids[-1] > CARD_MAX):
            raise DbError("A card number is outside what the device accepts")
        lines.append("#CARDS,%d" % len(ids))
        lines.extend("%010d" % i for i in ids)
    return "\r\n".join(lines) + "\r\n"


# --------------------------------------------------------------------------
# The database
# --------------------------------------------------------------------------

_SCHEMA = """
CREATE TABLE students (
    card_id     INTEGER PRIMARY KEY CHECK (card_id BETWEEN 1 AND 4294967039),
    student_no  TEXT NOT NULL DEFAULT '',
    name        TEXT NOT NULL CHECK (length(trim(name)) > 0),
    department  TEXT NOT NULL DEFAULT '',
    created_at  TEXT NOT NULL,
    updated_at  TEXT NOT NULL
);
CREATE TABLE modules (
    code        TEXT PRIMARY KEY CHECK (length(code) > 0),
    title       TEXT NOT NULL DEFAULT '',
    department  TEXT NOT NULL DEFAULT ''
);
CREATE TABLE enrollments (
    card_id     INTEGER NOT NULL REFERENCES students(card_id) ON DELETE CASCADE,
    module_code TEXT NOT NULL REFERENCES modules(code) ON DELETE CASCADE ON UPDATE CASCADE,
    PRIMARY KEY (card_id, module_code)
);
CREATE TABLE lectures (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    module_code TEXT NOT NULL REFERENCES modules(code) ON UPDATE CASCADE,
    title       TEXT NOT NULL,
    start_ts    INTEGER NOT NULL,
    end_ts      INTEGER,
    confirmed   INTEGER NOT NULL DEFAULT 0,
    created_at  TEXT NOT NULL
);
CREATE INDEX lectures_start ON lectures(start_ts);
CREATE TABLE taps (
    card_id     INTEGER NOT NULL,
    ts          INTEGER NOT NULL,
    device_id   INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (card_id, ts, device_id)
) WITHOUT ROWID;
CREATE INDEX taps_ts ON taps(ts);
CREATE TABLE kv (
    key         TEXT PRIMARY KEY,
    value       TEXT NOT NULL
);
CREATE TABLE sync_log (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    at          TEXT NOT NULL,
    device_id   INTEGER NOT NULL,
    rows_read   INTEGER NOT NULL,
    rows_new    INTEGER NOT NULL
);
"""


class Database:
    def __init__(self, path):
        self.path = path
        self.lock = threading.RLock()
        self.con = sqlite3.connect(path, check_same_thread=False, isolation_level=None)
        try:
            self.con.row_factory = sqlite3.Row
            self.con.execute("PRAGMA foreign_keys = ON")
            if path != ":memory:":
                self.con.execute("PRAGMA journal_mode = WAL")
                self.con.execute("PRAGMA synchronous = NORMAL")
            self._migrate()
        except Exception:
            self.con.close()            # a database we refuse to open must not stay open
            raise

    def close(self):
        with self.lock:
            self.con.close()

    def _migrate(self):
        with self.lock:
            v = self.con.execute("PRAGMA user_version").fetchone()[0]
            if v == 0:
                self.con.executescript("BEGIN;" + _SCHEMA + "PRAGMA user_version = %d; COMMIT;" % SCHEMA_VERSION)
            elif v > SCHEMA_VERSION:
                raise DbError("This database was made by a newer version of the app")

    def _now(self):
        return _dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    def _tx(self):
        return _Tx(self)

    # ------------------------------------------------------------ modules
    def upsert_module(self, code, title="", department=""):
        code = clean_device_text(code, MODULE_BYTES)
        if not code:
            raise DbError("Enter a module code")
        with self.lock, self._tx():
            self.con.execute(
                "INSERT INTO modules(code,title,department) VALUES(?,?,?) "
                "ON CONFLICT(code) DO UPDATE SET title=excluded.title, department=excluded.department",
                (code, clean_free_text(title), clean_free_text(department)))
        return self.get_module(code)

    def get_module(self, code):
        with self.lock:
            r = self.con.execute("SELECT * FROM modules WHERE code=?", (code,)).fetchone()
            return dict(r) if r else None

    def list_modules(self):
        with self.lock:
            rows = self.con.execute(
                "SELECT m.code, m.title, m.department, "
                "(SELECT COUNT(*) FROM enrollments e WHERE e.module_code=m.code) AS students, "
                "(SELECT COUNT(*) FROM lectures l WHERE l.module_code=m.code) AS lectures "
                "FROM modules m ORDER BY m.code").fetchall()
            return [dict(r) for r in rows]

    def rename_module(self, old, new):
        new = clean_device_text(new, MODULE_BYTES)
        if not new:
            raise DbError("Enter a module code")
        with self.lock, self._tx():
            if not self.get_module(old):
                raise DbError("No such module: " + old)
            if old != new and self.get_module(new):
                raise DbError("There is already a module called " + new)
            self.con.execute("UPDATE modules SET code=? WHERE code=?", (new, old))

    def delete_module(self, code, force=False):
        with self.lock, self._tx():
            n = self.con.execute("SELECT COUNT(*) FROM lectures WHERE module_code=?", (code,)).fetchone()[0]
            if n and not force:
                raise DbError("%s has %d lecture%s recorded. Delete those first, or remove the module with its lectures."
                              % (code, n, "" if n == 1 else "s"))
            if n:
                self.con.execute("DELETE FROM lectures WHERE module_code=?", (code,))
            self.con.execute("DELETE FROM modules WHERE code=?", (code,))

    # ----------------------------------------------------------- students
    def _modules_of(self, card_id):
        return [r[0] for r in self.con.execute(
            "SELECT module_code FROM enrollments WHERE card_id=? ORDER BY module_code", (card_id,))]

    def _ensure_module(self, code):
        code = clean_device_text(code, MODULE_BYTES)
        if not code:
            return None
        self.con.execute("INSERT OR IGNORE INTO modules(code,title,department) VALUES(?, '', '')", (code,))
        return code

    def upsert_student(self, card_id, name, student_no="", department="", modules=None):
        """Create or update a student. modules: a list of module codes to enrol in (None leaves enrolments alone)."""
        card_id = parse_card(card_id)
        name = clean_free_text(name)
        if not name:
            raise DbError("Enter the student's name")
        with self.lock, self._tx():
            now = self._now()
            self.con.execute(
                "INSERT INTO students(card_id,student_no,name,department,created_at,updated_at) VALUES(?,?,?,?,?,?) "
                "ON CONFLICT(card_id) DO UPDATE SET student_no=excluded.student_no, name=excluded.name, "
                "department=excluded.department, updated_at=excluded.updated_at",
                (card_id, clean_free_text(student_no, 40), name, clean_free_text(department), now, now))
            if modules is not None:
                self.con.execute("DELETE FROM enrollments WHERE card_id=?", (card_id,))
                for code in modules:
                    code = self._ensure_module(code)
                    if code:
                        self.con.execute("INSERT OR IGNORE INTO enrollments(card_id,module_code) VALUES(?,?)", (card_id, code))
        return self.get_student(card_id)

    def get_student(self, card_id):
        with self.lock:
            r = self.con.execute("SELECT * FROM students WHERE card_id=?", (card_id,)).fetchone()
            if not r:
                return None
            d = dict(r)
            d["modules"] = self._modules_of(card_id)
            return d

    def delete_student(self, card_id):
        with self.lock, self._tx():
            if not self.con.execute("SELECT 1 FROM students WHERE card_id=?", (card_id,)).fetchone():
                raise DbError("No such student")
            self.con.execute("DELETE FROM students WHERE card_id=?", (card_id,))

    def card_ids(self):
        """Every registered card number, ascending: what the device is given."""
        with self.lock:
            return [r[0] for r in self.con.execute("SELECT card_id FROM students ORDER BY card_id")]

    def list_students(self, q=None, module=None, department=None):
        sql = ("SELECT s.*, (SELECT COUNT(*) FROM taps t WHERE t.card_id=s.card_id) AS taps, "
               "(SELECT MAX(ts) FROM taps t WHERE t.card_id=s.card_id) AS last_tap FROM students s")
        where, args = [], []
        if module:
            where.append("s.card_id IN (SELECT card_id FROM enrollments WHERE module_code=?)")
            args.append(module)
        if department:
            where.append("s.department=?")
            args.append(department)
        if q:
            like = "%" + q.strip().lower().replace("%", "").replace("_", "") + "%"
            where.append("(lower(s.name) LIKE ? OR lower(s.student_no) LIKE ? OR lower(s.department) LIKE ? "
                         "OR printf('%010d', s.card_id) LIKE ? OR CAST(s.card_id AS TEXT) LIKE ?)")
            args += [like, like, like, like, like]
        if where:
            sql += " WHERE " + " AND ".join(where)
        sql += " ORDER BY lower(s.name), s.card_id"
        with self.lock:
            rows = [dict(r) for r in self.con.execute(sql, args)]
            enr = {}
            for r in self.con.execute("SELECT card_id, module_code FROM enrollments ORDER BY module_code"):
                enr.setdefault(r[0], []).append(r[1])
            for d in rows:
                d["modules"] = enr.get(d["card_id"], [])
                d["last_tap_text"] = fmt_ts(d["last_tap"]) if d["last_tap"] else ""
            return rows

    def departments(self):
        with self.lock:
            return [r[0] for r in self.con.execute(
                "SELECT DISTINCT department FROM students WHERE department<>'' UNION "
                "SELECT DISTINCT department FROM modules WHERE department<>'' ORDER BY 1")]

    def set_enrollment(self, code, card_ids, mode="add"):
        """mode: add, remove, or set (exactly these cards)."""
        if mode not in ("add", "remove", "set"):
            raise DbError("Unknown enrolment action")
        with self.lock, self._tx():
            if not self.get_module(code):
                raise DbError("No such module: " + code)
            ids = []
            for c in card_ids:
                c = parse_card(c)
                if not self.con.execute("SELECT 1 FROM students WHERE card_id=?", (c,)).fetchone():
                    raise DbError("Card %s is not a registered student" % fmt_card(c))
                ids.append(c)
            if mode == "set":
                self.con.execute("DELETE FROM enrollments WHERE module_code=?", (code,))
            for c in ids:
                if mode == "remove":
                    self.con.execute("DELETE FROM enrollments WHERE card_id=? AND module_code=?", (c, code))
                else:
                    self.con.execute("INSERT OR IGNORE INTO enrollments(card_id,module_code) VALUES(?,?)", (c, code))

    # ---- bulk import / export of the student list
    _HEAD = {
        "card_id": "card", "card": "card", "cardid": "card", "card_no": "card", "card_number": "card", "id": "card",
        "name": "name", "student_name": "name", "full_name": "name",
        "student_no": "no", "student_number": "no", "index": "no", "index_no": "no", "index_number": "no", "reg_no": "no", "registration": "no",
        "department": "dept", "dept": "dept",
        "modules": "modules", "module": "modules", "enrolled_modules": "modules", "enrolled": "modules",
    }

    def import_students(self, text, replace_modules=True):
        """Add or update students from CSV text. Returns {added, updated, errors:[{line,message}]}."""
        text = str(text).lstrip("\ufeff")
        sample = text[:2000]
        delim = "\t" if sample.count("\t") > sample.count(",") else ","
        rows = list(csv.reader(io.StringIO(text), delimiter=delim))
        rows = [r for r in rows if any(c.strip() for c in r)]
        result = {"added": 0, "updated": 0, "errors": []}
        if not rows:
            return result
        cols = None
        first = [c.strip().lower().replace(" ", "_") for c in rows[0]]
        if any(c in self._HEAD for c in first) and not re.fullmatch(r"(0[xX][0-9a-fA-F]+|\d+)", first[0] or "x"):
            cols = {}
            for i, c in enumerate(first):
                if c in self._HEAD and self._HEAD[c] not in cols:
                    cols[self._HEAD[c]] = i
            start = 1
            if "card" not in cols or "name" not in cols:
                raise DbError("The first line must name at least the columns card_id and name")
        else:
            cols = {"card": 0, "name": 1, "no": 2, "dept": 3, "modules": 4}
            start = 0
        for n, r in enumerate(rows[start:], start=start + 1):
            def cell(k):
                i = cols.get(k)
                return r[i].strip() if i is not None and i < len(r) else ""
            try:
                card = parse_card(cell("card"))
                existed = self.get_student(card) is not None
                mods = None
                if "modules" in cols:
                    raw = cell("modules")
                    mods = [m.strip() for m in re.split(r"[;|/]", raw) if m.strip()]
                    if not mods and not replace_modules:
                        mods = None
                self.upsert_student(card, cell("name"), cell("no"), cell("dept"), mods)
                result["updated" if existed else "added"] += 1
            except DbError as e:
                result["errors"].append({"line": n, "message": str(e)})
        return result

    def students_csv(self):
        buf = io.StringIO()
        w = csv.writer(buf, lineterminator="\r\n")
        w.writerow(["card_id", "name", "student_no", "department", "modules"])
        for s in self.list_students():
            w.writerow([fmt_card(s["card_id"]), s["name"], s["student_no"], s["department"], ";".join(s["modules"])])
        return buf.getvalue()

    # --------------------------------------------------------------- taps
    def add_taps(self, rows, device_id=0):
        """Store taps [(ts, card_id)]; ones already stored are skipped. Returns how many were new."""
        with self.lock, self._tx():
            before = self.con.total_changes
            self.con.executemany("INSERT OR IGNORE INTO taps(card_id,ts,device_id) VALUES(?,?,?)",
                                 [(c, t, device_id) for t, c in rows])
            new = self.con.total_changes - before
            self.con.execute("INSERT INTO sync_log(at,device_id,rows_read,rows_new) VALUES(?,?,?,?)",
                             (self._now(), device_id, len(rows), new))
        return new

    def tap_count(self):
        with self.lock:
            return self.con.execute("SELECT COUNT(*) FROM taps").fetchone()[0]

    def last_sync(self):
        with self.lock:
            r = self.con.execute("SELECT * FROM sync_log ORDER BY id DESC LIMIT 1").fetchone()
            return dict(r) if r else None

    def list_taps(self, ts_from=None, ts_to=None, card_id=None, limit=5000):
        sql = ("SELECT t.card_id, t.ts, s.name, s.student_no, s.department FROM taps t "
               "LEFT JOIN students s ON s.card_id=t.card_id")
        where, args = [], []
        if ts_from is not None:
            where.append("t.ts>=?"); args.append(ts_from)
        if ts_to is not None:
            where.append("t.ts<?"); args.append(ts_to)
        if card_id is not None:
            where.append("t.card_id=?"); args.append(card_id)
        if where:
            sql += " WHERE " + " AND ".join(where)
        sql += " ORDER BY t.ts DESC, t.card_id LIMIT ?"
        args.append(limit)
        with self.lock:
            out = []
            for r in self.con.execute(sql, args):
                d = dict(r)
                d["time"] = fmt_ts(d["ts"])
                out.append(d)
            return out

    def unregistered_cards(self):
        """Cards the device has recorded that belong to nobody yet, newest first."""
        with self.lock:
            rows = self.con.execute(
                "SELECT t.card_id, COUNT(*) AS taps, MIN(t.ts) AS first_ts, MAX(t.ts) AS last_ts FROM taps t "
                "WHERE t.card_id NOT IN (SELECT card_id FROM students) GROUP BY t.card_id ORDER BY last_ts DESC").fetchall()
            return [dict(r, first=fmt_ts(r["first_ts"]), last=fmt_ts(r["last_ts"])) for r in rows]

    def taps_csv(self, ts_from=None, ts_to=None):
        buf = io.StringIO()
        w = csv.writer(buf, lineterminator="\r\n")
        w.writerow(["date", "time", "card_id", "name", "student_no", "department"])
        for t in reversed(self.list_taps(ts_from, ts_to, limit=1000000)):
            w.writerow([t["time"][:10], t["time"][11:], fmt_card(t["card_id"]), t["name"] or "", t["student_no"] or "",
                        t["department"] or ""])
        return buf.getvalue()

    # ----------------------------------------------------------- lectures
    def _lecture_rows(self):
        return [dict(r) for r in self.con.execute("SELECT * FROM lectures ORDER BY start_ts, id")]

    @staticmethod
    def _window_end(lec, next_start):
        end = lec["end_ts"] if lec["end_ts"] is not None else lec["start_ts"] + MAX_LECTURE_SECONDS
        if next_start is not None:
            end = min(end, next_start)
        return max(end, lec["start_ts"])

    def _windows(self):
        """Every lecture with its effective [start, end) window, in time order."""
        rows = self._lecture_rows()
        out = []
        for i, l in enumerate(rows):
            nxt = rows[i + 1]["start_ts"] if i + 1 < len(rows) else None
            l["end"] = self._window_end(l, nxt)
            out.append(l)
        return out

    def _decorate(self, l):
        l["start_text"] = fmt_ts(l["start_ts"])
        l["end_text"] = fmt_ts(l["end"])
        l["date"] = fmt_date(l["start_ts"])
        l["running"] = l["end_ts"] is None
        return l

    def start_lecture(self, module, title, at=None):
        """Record that a lecture begins now (or at). The previous open lecture ends here."""
        module = clean_device_text(module, MODULE_BYTES)
        title = clean_device_text(title, LECTURE_BYTES)
        if not module:
            raise DbError("Choose or enter a module")
        if not title:
            raise DbError("Enter a lecture name")
        at = now_ts() if at is None else at
        with self.lock, self._tx():
            self._ensure_module(module)
            self.con.execute("UPDATE lectures SET end_ts=? WHERE end_ts IS NULL AND start_ts<=?", (at, at))
            cur = self.con.execute(
                "INSERT INTO lectures(module_code,title,start_ts,end_ts,confirmed,created_at) VALUES(?,?,?,NULL,0,?)",
                (module, title, at, self._now()))
            return self.get_lecture(cur.lastrowid)

    def create_lecture(self, module, title, start_ts, end_ts):
        module = clean_device_text(module, MODULE_BYTES)
        title = clean_free_text(title, 80)
        if not module:
            raise DbError("Choose or enter a module")
        if not title:
            raise DbError("Enter a lecture name")
        if end_ts is not None and end_ts <= start_ts:
            raise DbError("The lecture must end after it starts")
        with self.lock, self._tx():
            self._ensure_module(module)
            cur = self.con.execute(
                "INSERT INTO lectures(module_code,title,start_ts,end_ts,confirmed,created_at) VALUES(?,?,?,?,1,?)",
                (module, title, start_ts, end_ts, self._now()))
            return self.get_lecture(cur.lastrowid)

    def update_lecture(self, lecture_id, module=None, title=None, start_ts=None, end_ts="keep"):
        with self.lock, self._tx():
            cur = self.con.execute("SELECT * FROM lectures WHERE id=?", (lecture_id,)).fetchone()
            if not cur:
                raise DbError("No such lecture")
            m = clean_device_text(module, MODULE_BYTES) if module is not None else cur["module_code"]
            t = clean_free_text(title, 80) if title is not None else cur["title"]
            s = cur["start_ts"] if start_ts is None else start_ts
            e = cur["end_ts"] if end_ts == "keep" else end_ts
            if not m or not t:
                raise DbError("A lecture needs a module and a name")
            if e is not None and e <= s:
                raise DbError("The lecture must end after it starts")
            self._ensure_module(m)
            self.con.execute("UPDATE lectures SET module_code=?, title=?, start_ts=?, end_ts=?, confirmed=1 WHERE id=?",
                             (m, t, s, e, lecture_id))
        return self.get_lecture(lecture_id)

    def end_lecture(self, lecture_id, at=None):
        at = now_ts() if at is None else at
        with self.lock, self._tx():
            cur = self.con.execute("SELECT * FROM lectures WHERE id=?", (lecture_id,)).fetchone()
            if not cur:
                raise DbError("No such lecture")
            self.con.execute("UPDATE lectures SET end_ts=? WHERE id=?", (max(at, cur["start_ts"] + 1), lecture_id))
        return self.get_lecture(lecture_id)

    def delete_lecture(self, lecture_id):
        with self.lock, self._tx():
            if not self.con.execute("SELECT 1 FROM lectures WHERE id=?", (lecture_id,)).fetchone():
                raise DbError("No such lecture")
            self.con.execute("DELETE FROM lectures WHERE id=?", (lecture_id,))

    def get_lecture(self, lecture_id):
        with self.lock:
            for l in self._windows():
                if l["id"] == lecture_id:
                    return self._decorate(l)
        return None

    def confirm_lecture_start(self, module, title, since_ts):
        """
        The device says a lecture with these names began at since_ts by its own clock. If we started
        one with those names and have not yet heard back, take the device's time as the truth.
        Returns the lecture, or None if there was nothing to confirm.
        """
        module = clean_device_text(module, MODULE_BYTES)
        title = clean_device_text(title, LECTURE_BYTES)
        with self.lock, self._tx():
            r = self.con.execute(
                "SELECT * FROM lectures WHERE module_code=? AND title=? AND confirmed=0 ORDER BY id DESC LIMIT 1",
                (module, title)).fetchone()
            if not r:
                return None
            self.con.execute("UPDATE lectures SET start_ts=?, confirmed=1 WHERE id=?", (since_ts, r["id"]))
            # An earlier open lecture ended when this one was started; move that end with it.
            self.con.execute("UPDATE lectures SET end_ts=? WHERE end_ts=? AND id<>?", (since_ts, r["start_ts"], r["id"]))
        return self.get_lecture(r["id"])

    def adopt_device_lecture(self, module, title, since_ts):
        """
        The device ran a lecture this database has no record of (the lecturer started it on the device,
        someone edited SETTINGS.CSV by hand, or the database is new). Record it, once: a lecture deleted here is not resurrected on the
        next read, because only a lecture newer than the last one adopted is taken. One with no module
        name (the device started it before any lecture was named) is filed under UNASSIGNED_MODULE.
        Returns the new lecture, or None.
        """
        module = clean_device_text(module, MODULE_BYTES) or UNASSIGNED_MODULE
        title = clean_device_text(title, LECTURE_BYTES)
        if not title:
            return None
        with self.lock, self._tx():
            last = self.con.execute("SELECT value FROM kv WHERE key='adopted_since'").fetchone()
            if last is not None and since_ts <= int(last[0]):
                return None
            if self.con.execute("SELECT 1 FROM lectures WHERE module_code=? AND title=? AND start_ts=?",
                                (module, title, since_ts)).fetchone():
                self.con.execute("INSERT OR REPLACE INTO kv(key,value) VALUES('adopted_since',?)", (str(since_ts),))
                return None
            if module == UNASSIGNED_MODULE:
                self.con.execute("INSERT OR IGNORE INTO modules(code,title,department) VALUES(?,?,'')",
                                 (module, "Started on the device with no module: edit the lecture to move it"))
            self._ensure_module(module)
            # A lecture still open here ends where this one starts, but no later than its open window did:
            # read days afterwards, a device lecture must not stretch the one before it over the days between.
            self.con.execute("UPDATE lectures SET end_ts=MIN(?, start_ts+?) WHERE end_ts IS NULL AND start_ts<=?",
                             (since_ts, MAX_LECTURE_SECONDS, since_ts))
            cur = self.con.execute(
                "INSERT INTO lectures(module_code,title,start_ts,end_ts,confirmed,created_at) VALUES(?,?,?,NULL,1,?)",
                (module, title, since_ts, self._now()))
            self.con.execute("INSERT OR REPLACE INTO kv(key,value) VALUES('adopted_since',?)", (str(since_ts),))
            return self.get_lecture(cur.lastrowid)

    def note_confirmed(self, since_ts):
        """Remember that a lecture starting at since_ts is already known, so it is not adopted later."""
        with self.lock, self._tx():
            last = self.con.execute("SELECT value FROM kv WHERE key='adopted_since'").fetchone()
            if last is None or since_ts > int(last[0]):
                self.con.execute("INSERT OR REPLACE INTO kv(key,value) VALUES('adopted_since',?)", (str(since_ts),))

    def take_device_lectures(self, rows):
        """
        Every lecture start in the device's log (LECTURES.CSV: [(since_ts, module, title)]), oldest first:
        each confirms the lecture started here with those names, or is adopted as a lecture of its own, so
        lectures the device started by itself between two reads each get their taps. A row already
        recorded is passed over, and only the last row with a pair of names may confirm, so an older
        lecture of the same name never moves a newer one started here. Returns how many were added.
        """
        rows = sorted(rows, key=lambda r: r[0])
        names = [(clean_device_text(m, MODULE_BYTES) or UNASSIGNED_MODULE, clean_device_text(t, LECTURE_BYTES))
                 for _, m, t in rows]
        last = {n: i for i, n in enumerate(names)}
        added = 0
        with self.lock, self._tx():
            for i, ((since, _, _), (module, title)) in enumerate(zip(rows, names)):
                if self.con.execute("SELECT 1 FROM lectures WHERE module_code=? AND title=? AND start_ts=? AND confirmed=1",
                                    (module, title, since)).fetchone():
                    self.note_confirmed(since)
                elif last[(module, title)] == i and self.confirm_lecture_start(module, title, since):
                    self.note_confirmed(since)
                else:
                    lec = self.adopt_device_lecture(module, title, since)
                    if lec is None:
                        continue
                    added += 1
                    if i + 1 < len(rows):
                        # Another lecture followed on the device, so this one is over: not "running" here.
                        end = min(rows[i + 1][0], since + MAX_LECTURE_SECONDS)
                        if end > since:
                            self.con.execute("UPDATE lectures SET end_ts=? WHERE id=?", (end, lec["id"]))
        return added

    def current_lecture(self):
        """The lecture that is still open, if any."""
        with self.lock:
            for l in reversed(self._windows()):
                if l["end_ts"] is None:
                    return self._decorate(l)
        return None

    def list_lectures(self, module=None, date_from=None, date_to=None):
        """Lectures newest first, each with attendance counts. date_from/date_to: 'YYYY-MM-DD' inclusive."""
        lo = parse_ts(date_from) if date_from else None
        hi = parse_ts(date_to) + 86400 if date_to else None
        with self.lock:
            out = []
            for l in self._windows():
                if module and l["module_code"] != module:
                    continue
                if lo is not None and l["start_ts"] < lo:
                    continue
                if hi is not None and l["start_ts"] >= hi:
                    continue
                self._decorate(l)
                l["counts"] = self._counts(l)
                out.append(l)
            out.reverse()
            return out

    def _counts(self, l):
        rows = self.con.execute(
            "SELECT t.card_id FROM taps t WHERE t.ts>=? AND t.ts<? GROUP BY t.card_id", (l["start_ts"], l["end"])).fetchall()
        cards = {r[0] for r in rows}
        reg = {r[0] for r in self.con.execute("SELECT card_id FROM students")}
        enrolled = {r[0] for r in self.con.execute("SELECT card_id FROM enrollments WHERE module_code=?", (l["module_code"],))}
        pe = len(cards & enrolled)
        return {"enrolled": len(enrolled), "present_enrolled": pe, "present_other": len((cards & reg) - enrolled),
                "absent": len(enrolled - cards), "unregistered": len(cards - reg), "taps": len(cards),
                "percent": round(1000.0 * pe / len(enrolled)) / 10 if enrolled else 0.0}

    def lecture_attendance(self, lecture_id):
        """Who was there, who was not, and which cards belong to nobody."""
        with self.lock:
            l = None
            for w in self._windows():
                if w["id"] == lecture_id:
                    l = self._decorate(w)
            if not l:
                raise DbError("No such lecture")
            first = {r[0]: r[1] for r in self.con.execute(
                "SELECT card_id, MIN(ts) FROM taps WHERE ts>=? AND ts<? GROUP BY card_id", (l["start_ts"], l["end"]))}
            studs = {r["card_id"]: dict(r) for r in self.con.execute("SELECT * FROM students")}
            enrolled = {r[0] for r in self.con.execute("SELECT card_id FROM enrollments WHERE module_code=?", (l["module_code"],))}
            present, other, unreg = [], [], []
            for card, ts in first.items():
                s = studs.get(card)
                if s is None:
                    unreg.append({"card_id": card, "time": fmt_ts(ts)[11:], "ts": ts})
                    continue
                entry = {"card_id": card, "name": s["name"], "student_no": s["student_no"], "department": s["department"],
                         "time": fmt_ts(ts)[11:], "ts": ts, "enrolled": card in enrolled}
                (present if card in enrolled else other).append(entry)
            absent = [{"card_id": c, "name": studs[c]["name"], "student_no": studs[c]["student_no"],
                       "department": studs[c]["department"]} for c in enrolled if c in studs and c not in first]
            key = lambda e: (e["name"].lower(), e["card_id"])
            present.sort(key=key); other.sort(key=key); absent.sort(key=key)
            unreg.sort(key=lambda e: e["ts"])
            mod = self.get_module(l["module_code"]) or {}
            return {"lecture": l, "module_title": mod.get("title", ""), "present": present, "present_other": other,
                    "absent": absent, "unregistered": unreg, "counts": self._counts(l)}

    def unassigned_days(self):
        """Taps that fall in no lecture, grouped by day: attendance taken without starting a lecture."""
        with self.lock:
            wins = self._windows()
            starts = [w["start_ts"] for w in wins]
            days = {}
            for ts, card in self.con.execute("SELECT ts, card_id FROM taps ORDER BY ts"):
                i = bisect.bisect_right(starts, ts) - 1
                if i >= 0 and ts < wins[i]["end"]:
                    continue
                d = days.setdefault(fmt_date(ts), {"date": fmt_date(ts), "first_ts": ts, "last_ts": ts, "cards": set(), "taps": 0})
                d["last_ts"] = ts
                d["cards"].add(card)
                d["taps"] += 1
            out = []
            for d in sorted(days.values(), key=lambda x: x["date"], reverse=True):
                out.append({"date": d["date"], "first": fmt_ts(d["first_ts"])[11:], "last": fmt_ts(d["last_ts"])[11:],
                            "cards": len(d["cards"]), "taps": d["taps"]})
            return out

    def lecture_from_unassigned(self, date, module, title):
        """Turn a day's loose taps into a lecture covering them."""
        with self.lock:
            lo = parse_ts(date)
            wins = self._windows()
            starts = [w["start_ts"] for w in wins]
            mine = []
            for (ts,) in self.con.execute("SELECT ts FROM taps WHERE ts>=? AND ts<? ORDER BY ts", (lo, lo + 86400)):
                i = bisect.bisect_right(starts, ts) - 1
                if i >= 0 and ts < wins[i]["end"]:
                    continue
                mine.append(ts)
            if not mine:
                raise DbError("There are no unassigned taps on " + date)
            return self.create_lecture(module, title, mine[0], mine[-1] + 1)

    # ------------------------------------------------------------ reports
    def module_report(self, code, date_from=None, date_to=None):
        with self.lock:
            mod = self.get_module(code)
            if not mod:
                raise DbError("No such module: " + str(code))
            lectures = sorted(self.list_lectures(module=code, date_from=date_from, date_to=date_to), key=lambda l: l["start_ts"])
            studs = {r["card_id"]: dict(r) for r in self.con.execute("SELECT * FROM students")}
            enrolled = [r[0] for r in self.con.execute("SELECT card_id FROM enrollments WHERE module_code=?", (code,))]
            sets = []
            for l in lectures:
                sets.append({r[0] for r in self.con.execute(
                    "SELECT DISTINCT card_id FROM taps WHERE ts>=? AND ts<?", (l["start_ts"], l["end"]))})
            rows = []
            for c in enrolled:
                s = studs.get(c)
                if not s:
                    continue
                flags = [1 if c in st else 0 for st in sets]
                n = sum(flags)
                rows.append({"card_id": c, "name": s["name"], "student_no": s["student_no"], "department": s["department"],
                             "flags": flags, "count": n,
                             "percent": round(1000.0 * n / len(lectures)) / 10 if lectures else 0.0})
            rows.sort(key=lambda r: (r["name"].lower(), r["card_id"]))
            avg = round(10.0 * sum(r["percent"] for r in rows) / len(rows)) / 10 if rows and lectures else 0.0
            return {"module": mod, "from": date_from or "", "to": date_to or "",
                    "lectures": [{"id": l["id"], "title": l["title"], "date": l["date"], "start": l["start_text"],
                                  "present": l["counts"]["present_enrolled"], "enrolled": l["counts"]["enrolled"]} for l in lectures],
                    "students": rows,
                    "summary": {"lectures": len(lectures), "students": len(rows), "average": avg,
                                "below_75": sum(1 for r in rows if r["percent"] < 75)}}

    def student_report(self, card_id):
        with self.lock:
            s = self.get_student(card_id)
            if not s:
                raise DbError("No such student")
            wins = self._windows()
            taps = [r[0] for r in self.con.execute("SELECT ts FROM taps WHERE card_id=? ORDER BY ts", (card_id,))]
            attended = {}
            for ts in taps:
                for w in wins:
                    if w["start_ts"] <= ts < w["end"]:
                        attended.setdefault(w["id"], ts)
                        break
            modules = []
            for code in s["modules"]:
                ls = [w for w in wins if w["module_code"] == code]
                items = [{"id": w["id"], "title": w["title"], "date": fmt_date(w["start_ts"]),
                          "attended": w["id"] in attended, "time": fmt_ts(attended[w["id"]])[11:] if w["id"] in attended else ""}
                         for w in ls]
                n = sum(1 for i in items if i["attended"])
                modules.append({"code": code, "title": (self.get_module(code) or {}).get("title", ""), "lectures": items,
                                "attended": n, "total": len(items),
                                "percent": round(1000.0 * n / len(items)) / 10 if items else 0.0})
            return {"student": s, "modules": modules, "taps": len(taps),
                    "last_tap": fmt_ts(taps[-1]) if taps else ""}

    # ----------------------------------------------------------- exports
    @staticmethod
    def lecture_csv(att):
        l = att["lecture"]
        buf = io.StringIO()
        w = csv.writer(buf, lineterminator="\r\n")
        c = att["counts"]
        w.writerow(["Module", l["module_code"]])
        w.writerow(["Lecture", l["title"]])
        w.writerow(["Date", l["date"]])
        w.writerow(["Time", l["start_text"][11:16] + "-" + l["end_text"][11:16]])
        w.writerow(["Present", "%d of %d (%s%%)" % (c["present_enrolled"], c["enrolled"], c["percent"])])
        w.writerow([])
        w.writerow(["CARD_ID", "NAME", "STUDENT_NO", "DEPARTMENT", "STATUS", "TIME"])
        for p in att["present"]:
            w.writerow([fmt_card(p["card_id"]), p["name"], p["student_no"], p["department"], "Present", p["time"]])
        for p in att["present_other"]:
            w.writerow([fmt_card(p["card_id"]), p["name"], p["student_no"], p["department"], "Present (not enrolled)", p["time"]])
        for a in att["absent"]:
            w.writerow([fmt_card(a["card_id"]), a["name"], a["student_no"], a["department"], "Absent", ""])
        for u in att["unregistered"]:
            w.writerow([fmt_card(u["card_id"]), "", "", "", "Unregistered card", u["time"]])
        return buf.getvalue()

    @staticmethod
    def module_csv(rep):
        buf = io.StringIO()
        w = csv.writer(buf, lineterminator="\r\n")
        w.writerow(["CARD_ID", "NAME", "STUDENT_NO", "DEPARTMENT"] + [l["date"] + " " + l["title"] for l in rep["lectures"]] +
                   ["PRESENT", "PERCENT"])
        for s in rep["students"]:
            w.writerow([fmt_card(s["card_id"]), s["name"], s["student_no"], s["department"]] +
                       ["P" if f else "A" for f in s["flags"]] + ["%d/%d" % (s["count"], len(rep["lectures"])), s["percent"]])
        return buf.getvalue()

    @staticmethod
    def student_csv(rep):
        s = rep["student"]
        buf = io.StringIO()
        w = csv.writer(buf, lineterminator="\r\n")
        w.writerow(["Name", s["name"]])
        w.writerow(["Card", fmt_card(s["card_id"])])
        w.writerow(["Student no", s["student_no"]])
        w.writerow(["Department", s["department"]])
        w.writerow([])
        w.writerow(["MODULE", "DATE", "LECTURE", "ATTENDED", "TIME"])
        for m in rep["modules"]:
            for l in m["lectures"]:
                w.writerow([m["code"], l["date"], l["title"], "Yes" if l["attended"] else "No", l["time"]])
        return buf.getvalue()

    # ------------------------------------------------------------- misc
    def counts(self):
        with self.lock:
            q = lambda s: self.con.execute(s).fetchone()[0]
            return {"students": q("SELECT COUNT(*) FROM students"), "modules": q("SELECT COUNT(*) FROM modules"),
                    "lectures": q("SELECT COUNT(*) FROM lectures"), "taps": q("SELECT COUNT(*) FROM taps"),
                    "unregistered": q("SELECT COUNT(DISTINCT card_id) FROM taps WHERE card_id NOT IN (SELECT card_id FROM students)")}

    def backup_bytes(self):
        """A consistent copy of the whole database as one file."""
        with self.lock:
            import os
            import tempfile
            fd, tmp = tempfile.mkstemp(suffix=".db")
            os.close(fd)
            try:
                dst = sqlite3.connect(tmp)
                self.con.backup(dst)
                dst.close()
                with open(tmp, "rb") as f:
                    return f.read()
            finally:
                try:
                    os.remove(tmp)
                except OSError:
                    pass


class _Tx:
    """BEGIN ... COMMIT (or ROLLBACK), re-entrant so methods can call each other."""

    def __init__(self, db):
        self.db = db

    def __enter__(self):
        d = self.db
        n = getattr(d, "_depth", 0)
        if n == 0:
            d.con.execute("BEGIN")
        d._depth = n + 1

    def __exit__(self, et, ev, tb):
        d = self.db
        d._depth -= 1
        if d._depth == 0:
            d.con.execute("ROLLBACK" if et else "COMMIT")
        return False

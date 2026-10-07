#!/usr/bin/env python3
"""
Attendance Logger companion app: the desktop window.

A simple, colourful window over the same Python that the browser version
(attendance_app.py) uses. Two tabs:

    Lectures   start a lecture, and for each lecture see who came: index number,
               name and the time they tapped
    Students   register cards: index number and name

and three buttons for the device itself: read it now, set its clock, and clear
the records in its memory (after copying every one of them to this computer).

It needs only Python 3.8 or newer with Tk (included in the python.org installers
for Windows and macOS; on Linux it is the python3-tk package).

    python attendance_gui.py                     normal use
    python attendance_gui.py --demo              try it with made-up data, no device
    python attendance_gui.py --data-dir D:\\Attendance    keep the database elsewhere
    python attendance_gui.py --device-dir FOLDER  use a folder as the device

Device scanning and anything written to the device run on a worker thread; the
results come back to the window through a queue, so the window never freezes
while Windows wakes up a slow drive.
"""
import argparse
import csv
import io
import json
import os
import queue
import re
import shutil
import sys
import tempfile
import threading

try:
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
    from tkinter import font as tkfont
except ImportError:          # a Python without Tk: main() says what to do
    tk = None

import attendance_app as A
import attendance_db as D

APP_TITLE = "Attendance Logger"
POLL_MS = 2000
LOCK_NAME = "companion.lock"
PREFS_NAME = "companion-gui.json"
DRIFT_LIMIT = 120              # seconds before the clock is called wrong, as in the browser version
DEFAULT_MODULE = "GENERAL"     # the device stores a module with every lecture; this window does not ask for one

PAL = {                        # the window's colours
    "bg": "#F4F6FC", "card": "#FFFFFF", "line": "#D7DBEC", "text": "#1E2235", "muted": "#5E6478",
    "stripe": "#F3F5FD", "select": "#C9D2F5", "toolbar": "#E6E9F8",
    "indigo": "#3F51B5", "indigo_dark": "#283593", "blue": "#1E88E5", "blue_dark": "#1565C0",
    "teal": "#00897B", "teal_dark": "#00695C", "green": "#43A047", "green_dark": "#2E7D32",
    "green_soft": "#E8F5E9", "red": "#E53935", "red_dark": "#C62828", "orange": "#FB8C00",
    "orange_dark": "#EF6C00", "amber_soft": "#FFF4E0", "amber_text": "#8A4B00", "purple": "#8E24AA",
    "purple_dark": "#6A1B9A",
}
BUTTONS = (                    # coloured button styles: name, colour, colour when pressed
    ("Indigo", "indigo", "indigo_dark"), ("Blue", "blue", "blue_dark"), ("Teal", "teal", "teal_dark"),
    ("Green", "green", "green_dark"), ("Red", "red", "red_dark"), ("Orange", "orange", "orange_dark"),
    ("Purple", "purple", "purple_dark"),
)
COLORS = {                     # banner and status colours: background, text
    "ok": ("#E8F5E9", "#1B5E20"),
    "info": ("#E3F2FD", "#0D47A1"),
    "warn": ("#FFF4E0", "#7A4100"),
    "bad": ("#FDECEA", "#8A1F17"),
    "off": ("#ECEFF1", "#37474F"),
}
DOTS = {"ok": "#2E7D32", "wait": "#EF6C00", "off": "#78909C"}


# --------------------------------------------------------------------------
# Plain helpers (no Tk; the tests use them directly)
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


def lecture_csv(detail):
    """A lecture's attendance as CSV text: index number, name, card, time; unregistered cards last."""
    out = io.StringIO()
    w = csv.writer(out, lineterminator="\r\n")
    w.writerow(["Index No", "Name", "Card", "Time"])
    for s in present_students(detail):
        w.writerow([s["student_no"], s["name"], card10(s["card_id"]), s["time"]])
    for u in detail["unregistered"]:
        w.writerow(["", "(card not registered)", card10(u["card_id"]), u["time"]])
    return out.getvalue()


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
    return "wait", "Waiting for the device", "plug it in with the USB-C cable"


def cards_line(st):
    """What the Students tab says about the device's card list ('' when it does not say)."""
    cs = st.get("cards") if st and st.get("connected") else None
    if not cs or cs.get("device") is None:
        return ""
    if cs["in_sync"]:
        return "The device knows all %d cards." % cs["device"]
    return "The device knows %d of %d cards. Send them so new cards show green." % (cs["device"], cs["database"])


def banner_list(st, sent, tab=None, dismissed=()):
    """The banners under the toolbar. Each is {key, kind, title, text, action}; a banner with an action
    ('send-cards' or 'eject') carries a button and cannot be dismissed."""
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
        cs = st.get("cards")
        if cs and cs.get("too_many"):
            add("cardsmany", "bad", "There are more students than the device can hold.",
                "It holds %d cards and the list has %d. Delete the students who have left, then send the cards."
                % (D.DEVICE_CARDS_MAX, cs["database"]))
        elif cs and cs.get("in_sync") is False and not d.get("pending"):
            add("cards", "warn", "The device does not have your latest student cards.",
                "It knows %d and the list has %d. Until it is updated, new cards show red when tapped."
                % (cs["device"], cs["database"]), action="send-cards")
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
            "Now eject the drive, or press the button on the device once. The device then checks the file: green "
            "light and two buzzes means started, red light and three buzzes means it was refused.", action="eject")
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
    @results as (callback, value) for the Tk thread to call; this thread never touches a widget."""

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


# --------------------------------------------------------------------------
# Widgets
# --------------------------------------------------------------------------

def col(key, heading, width, anchor="w", stretch=True):
    return (key, heading, width, anchor, stretch)


if tk is not None:

    class Table(ttk.Frame):
        """A striped Treeview with a scroll bar; clicking a column heading sorts by it (again: the other way)."""

        def __init__(self, parent, columns=(), height=10, sortable=True, selectmode="browse"):
            ttk.Frame.__init__(self, parent)
            self.tree = ttk.Treeview(self, show="headings", height=height, selectmode=selectmode)
            ys = ttk.Scrollbar(self, orient="vertical", command=self.tree.yview)
            self.tree.configure(yscrollcommand=ys.set)
            self.tree.grid(row=0, column=0, sticky="nsew")
            ys.grid(row=0, column=1, sticky="ns")
            self.columnconfigure(0, weight=1)
            self.rowconfigure(0, weight=1)
            self.tree.tag_configure("stripe", background=PAL["stripe"])
            self.tree.tag_configure("running", foreground=PAL["green_dark"])
            self.sortable = sortable
            self.sort_key, self.sort_desc = None, False
            self.cols, self.rows = [], []
            self.empty = ttk.Label(self, style="Card.Muted.TLabel", justify="center", wraplength=380)
            if columns:
                self.set_columns(columns)

        def set_columns(self, columns):
            self.cols = list(columns)
            keys = [c[0] for c in self.cols]
            self.tree.configure(columns=keys, displaycolumns=keys)
            for key, head, width, anchor, stretch in self.cols:
                self.tree.heading(key, text=head, anchor=anchor,
                                  command=(lambda k=key: self.sort_by(k)) if self.sortable else "")
                self.tree.column(key, width=width, minwidth=40, anchor=anchor, stretch=stretch)
            self._headings()

        def _headings(self):
            for key, head, _, _, _ in self.cols:
                mark = (" \u25bc" if self.sort_desc else " \u25b2") if key == self.sort_key else ""
                self.tree.heading(key, text=head + mark)

        def sort_by(self, key):
            if self.sort_key == key:
                self.sort_desc = not self.sort_desc
            else:
                self.sort_key, self.sort_desc = key, False
            self.fill(self.rows, self.empty.cget("text"))

        def _sorted(self, rows):
            if not self.sort_key:
                return rows
            idx = [c[0] for c in self.cols].index(self.sort_key)

            def k(row):
                v = row[1][idx] if idx < len(row[1]) else ""
                try:
                    return (0, float(str(v).strip()), "")
                except ValueError:
                    return (1, 0.0, str(v).lower())
            return sorted(rows, key=k, reverse=self.sort_desc)

        def fill(self, rows, empty_text=""):
            """rows: [(iid, values, tags)]. The selection is kept where its rows still exist."""
            self.rows = list(rows)
            t = self.tree
            sel, focus = t.selection(), t.focus()
            t.delete(*t.get_children())
            for i, (iid, values, tags) in enumerate(self._sorted(self.rows)):
                t.insert("", "end", iid=iid, values=[("" if v is None else v) for v in values],
                         tags=tuple(tags) + (("stripe",) if i % 2 else ()))
            keep = [i for i in sel if t.exists(i)]
            if keep:
                t.selection_set(keep)
            if focus and t.exists(focus):
                t.focus(focus)
            self._headings()
            self.empty.configure(text=empty_text)
            if not self.rows and empty_text:
                self.empty.place(relx=0.5, rely=0.45, anchor="center")
            else:
                self.empty.place_forget()

        def selected(self):
            sel = self.tree.selection()
            return sel[0] if sel else None

        def select(self, iid):
            if iid and self.tree.exists(iid):
                self.tree.selection_set(iid)
                self.tree.focus(iid)
                self.tree.see(iid)

        def on_activate(self, fn):
            """Double-click or Enter on a row."""
            def dbl(e):
                if self.tree.identify_region(e.x, e.y) in ("cell", "tree"):
                    iid = self.tree.identify_row(e.y)
                    if iid:
                        fn(iid)

            def enter(e):
                iid = self.selected()
                if iid:
                    fn(iid)
                return "break"
            self.tree.bind("<Double-1>", dbl)
            self.tree.bind("<Return>", enter)

    def wrapping(label, container, margin=24, minimum=200):
        """Keep @label's lines as wide as @container."""
        container.bind("<Configure>", lambda e: label.configure(wraplength=max(minimum, e.width - margin)), add="+")
        return label

    class Dialog(tk.Toplevel):
        """A modal window: Enter saves, Escape cancels."""

        def __init__(self, win, title):
            tk.Toplevel.__init__(self, win.root)
            self.withdraw()
            self.win = win
            self.title(title)
            self.configure(bg=PAL["bg"])
            self.transient(win.root)
            self.body = ttk.Frame(self, padding=18)
            self.body.pack(fill="both", expand=True)
            self.body.columnconfigure(0, weight=1)
            self.body.columnconfigure(1, weight=1)
            self.protocol("WM_DELETE_WINDOW", self.cancel)
            self.bind("<Escape>", lambda e: self.cancel())
            self.bind("<Return>", self._enter)

        def _enter(self, e):
            if isinstance(e.widget, tk.Text) or str(e.widget.winfo_class()) == "Treeview":
                return None
            if isinstance(e.widget, ttk.Button):
                e.widget.invoke()
                return "break"
            self.ok()
            return "break"

        def ok(self):
            pass

        def buttons(self, row, buttons, danger=None):
            f = ttk.Frame(self.body)
            f.grid(row=row, column=0, columnspan=2, sticky="ew", pady=(16, 0))
            if danger:
                ttk.Button(f, text=danger[0], style="Red.TButton", command=danger[1]).pack(side="left")
            for text, cmd, primary in reversed(buttons):
                ttk.Button(f, text=text, command=cmd, style="Indigo.TButton" if primary else "TButton").pack(
                    side="right", padx=(6, 0))
            return f

        def show(self, focus=None):
            self.update_idletasks()
            r = self.win.root
            w, h = self.winfo_reqwidth(), self.winfo_reqheight()
            x = r.winfo_rootx() + max(0, (r.winfo_width() - w) // 2)
            y = r.winfo_rooty() + max(0, (r.winfo_height() - h) // 3)
            self.geometry("+%d+%d" % (x, y))
            self.deiconify()
            self._grab(20)
            if focus is not None:
                focus.focus_set()

        def _grab(self, tries):
            if not self.winfo_exists():
                return
            try:
                self.grab_set()
            except tk.TclError:              # not viewable yet
                if tries:
                    self.win.root.after(50, lambda: self._grab(tries - 1))

        def cancel(self):
            try:
                self.grab_release()
            except tk.TclError:
                pass
            self.destroy()

        def fail(self, e, label):
            label.configure(text=err_text(e))
            self.bell()

        def confirm(self, text):
            return messagebox.askyesno(APP_TITLE, text, parent=self, icon="warning")

    # ----------------------------------------------------------------- dialogs
    class StudentDialog(Dialog):
        """Register a card, or edit a student: card number, name and index number."""

        def __init__(self, win, card=None, prefill=None):
            s = win.db.get_student(card) if card is not None else None
            Dialog.__init__(self, win, "Edit student" if s else "Register a student")
            self.student = s
            v = s or prefill or {}
            b = self.body
            ttk.Label(b, text="Edit student" if s else "Register a student", style="H2.TLabel").grid(
                row=0, column=0, columnspan=2, sticky="w")
            ttk.Label(b, text="Card number").grid(row=1, column=0, columnspan=2, sticky="w", pady=(12, 2))
            cr = ttk.Frame(b)
            cr.grid(row=2, column=0, columnspan=2, sticky="ew")
            self.card = tk.StringVar(value=card10(v["card_id"]) if v.get("card_id") else "")
            self.e_card = ttk.Entry(cr, textvariable=self.card, width=18)
            self.e_card.pack(side="left")
            if s:
                self.e_card.state(["readonly"])
            else:
                ttk.Button(cr, text="Read from device", command=self.wait_card).pack(side="left", padx=(6, 0))
                if win.cards:
                    self.pick = ttk.Combobox(cr, state="readonly", width=30, values=[
                        "%s (last %s)" % (card10(c["card_id"]), c["last"][5:16]) for c in win.cards])
                    self.pick.set("Pick a card the device saw\u2026")
                    self.pick.pack(side="left", padx=(6, 0))
                    self.pick.bind("<<ComboboxSelected>>", self._picked)
            self.waiting_lbl = ttk.Label(b, style="Info.TLabel", wraplength=480, text=(
                "Waiting for a new card\u2026 Tap it on the device, then plug the device into this computer. "
                "The number will appear here."))
            self.card_err = ttk.Label(b, style="Err.TLabel")
            self.card_err.grid(row=4, column=0, columnspan=2, sticky="w")
            self.card.trace_add("write", self._check_card)

            ttk.Label(b, text="Index number").grid(row=5, column=0, sticky="w", pady=(8, 2))
            ttk.Label(b, text="Name").grid(row=5, column=1, sticky="w", pady=(8, 2), padx=(10, 0))
            self.no = tk.StringVar(value=v.get("student_no", ""))
            self.e_no = ttk.Entry(b, textvariable=self.no, width=18, validate="key", validatecommand=win.vcmd(40))
            self.e_no.grid(row=6, column=0, sticky="ew")
            self.name = tk.StringVar(value=v.get("name", ""))
            self.e_name = ttk.Entry(b, textvariable=self.name, width=36, validate="key", validatecommand=win.vcmd(80))
            self.e_name.grid(row=6, column=1, sticky="ew", padx=(10, 0))
            self.err = ttk.Label(b, style="Err.TLabel", wraplength=480)
            self.err.grid(row=7, column=0, columnspan=2, sticky="w", pady=(8, 0))
            self.buttons(8, [("Cancel", self.cancel, False), ("Save", self.ok, True)],
                         danger=("Delete student", self.delete) if s else None)
            self.show(self.e_no if (s or v.get("card_id")) else self.e_card)

        def _picked(self, e=None):
            i = self.pick.current()
            if i >= 0:
                self.card.set(card10(self.win.cards[i]["card_id"]))
                self.e_no.focus_set()

        def _check_card(self, *a):
            v = self.card.get().strip()
            bad = v and not re.fullmatch(r"(0[xX][0-9a-fA-F]+|\d+)", v)
            self.card_err.configure(text="A card number is digits only, like 0000123456." if bad else "")

        def wait_card(self):
            self.win.waiting = {"known": {c["card_id"] for c in self.win.cards}, "dialog": self}
            self.waiting_lbl.grid(row=3, column=0, columnspan=2, sticky="w", pady=(6, 0))

        def card_read(self, card_id):
            self.card.set(card10(card_id))
            self.waiting_lbl.grid_remove()

        def ok(self):
            self.err.configure(text="")
            # This window does not show departments or modules: whatever a student already has is kept.
            dept = (self.student or {}).get("department", "")
            try:
                s = self.win.db.upsert_student(self.card.get(), self.name.get(), self.no.get(), dept, None)
            except D.DbError as e:
                return self.fail(e, self.err)
            self.cancel()
            self.win.changed(s["name"] + " saved. Send the cards to the device so it shows green for this card.",
                             poll=True)

        def delete(self):
            if not self.confirm("Delete this student? Their taps stay, and show as an unregistered card."):
                return
            try:
                self.win.db.delete_student(self.student["card_id"])
            except D.DbError as e:
                return self.fail(e, self.err)
            self.cancel()
            self.win.changed("Student deleted")

        def destroy(self):
            if self.win.waiting and self.win.waiting.get("dialog") is self:
                self.win.waiting = None
            Dialog.destroy(self)

    class ImportDialog(Dialog):
        """A class list pasted from Excel or read from a CSV file."""

        def __init__(self, win):
            Dialog.__init__(self, win, "Import students")
            b = self.body
            ttk.Label(b, text="Import students", style="H2.TLabel").grid(row=0, column=0, columnspan=2, sticky="w")
            ttk.Label(b, wraplength=560, text="Paste rows from Excel, or choose a CSV file, with the columns: card "
                      "number, name, index number. A first line naming the columns (card_id, name, index_no) is "
                      "fine too. Students already in the list are updated.").grid(
                row=1, column=0, columnspan=2, sticky="w", pady=(6, 6))
            tf = ttk.Frame(b)
            tf.grid(row=2, column=0, columnspan=2, sticky="nsew")
            self.text = tk.Text(tf, width=70, height=11, wrap="none", undo=True, font="TkFixedFont",
                                relief="flat", highlightthickness=1, highlightbackground=PAL["line"],
                                highlightcolor=PAL["indigo"])
            ys = ttk.Scrollbar(tf, orient="vertical", command=self.text.yview)
            self.text.configure(yscrollcommand=ys.set)
            self.text.pack(side="left", fill="both", expand=True)
            ys.pack(side="right", fill="y")
            fr = ttk.Frame(b)
            fr.grid(row=3, column=0, columnspan=2, sticky="w", pady=(6, 0))
            ttk.Button(fr, text="Choose a file\u2026", command=self.pick).pack(side="left")
            self.file_lbl = ttk.Label(fr, style="Muted.TLabel")
            self.file_lbl.pack(side="left", padx=(8, 0))
            self.result = ttk.Label(b, wraplength=560, justify="left")
            self.result.grid(row=4, column=0, columnspan=2, sticky="w", pady=(8, 0))
            self.buttons(5, [("Close", self.cancel, False), ("Import", self.ok, True)])
            self.text.bind("<Control-Return>", lambda e: (self.ok(), "break")[1])
            self.show(self.text)

        def pick(self):
            path = filedialog.askopenfilename(parent=self, title="Choose a class list",
                                              filetypes=[("CSV or text", "*.csv *.CSV *.txt *.TXT"), ("All files", "*")])
            if not path:
                return
            try:
                text = A.read_text(path)
            except OSError as e:
                return self.fail(e, self.result)
            self.text.delete("1.0", "end")
            self.text.insert("1.0", text)
            self.file_lbl.configure(text=os.path.basename(path))

        def ok(self):
            text = self.text.get("1.0", "end-1c")
            # Columns by position: card, name, index number (the database's own order is card, name, number).
            if text.strip() and not re.match(r"^\s*[A-Za-z_]", text):
                rows = list(csv.reader(io.StringIO(text), delimiter="\t" if text.count("\t") > text.count(",") else ","))
                text = "card_id,name,index_no\n" + "\n".join(
                    ",".join(c.replace(",", " ") for c in (r + ["", "", ""])[:3]) for r in rows if any(x.strip() for x in r))
            try:
                r = self.win.db.import_students(text, replace_modules=False)
            except D.DbError as e:
                self.result.configure(style="Err.TLabel")
                return self.fail(e, self.result)
            msg = "%s added, %d updated." % (plural(r["added"], "student"), r["updated"])
            errs = ["Line %d: %s" % (x["line"], x["message"]) for x in r["errors"][:8]]
            if len(r["errors"]) > 8:
                errs.append("\u2026and %d more" % (len(r["errors"]) - 8))
            self.result.configure(text="\n".join([msg] + errs), style="Err.TLabel" if errs else "Bold.TLabel")
            self.win.changed(None)

    class RenameDialog(Dialog):
        """Give a lecture another name (a lecture started on the device is named by counting on)."""

        def __init__(self, win, lecture):
            Dialog.__init__(self, win, "Rename lecture")
            self.lecture = lecture
            b = self.body
            ttk.Label(b, text="Rename lecture", style="H2.TLabel").grid(row=0, column=0, columnspan=2, sticky="w")
            ttk.Label(b, text="Lecture name").grid(row=1, column=0, columnspan=2, sticky="w", pady=(12, 2))
            self.title_var = tk.StringVar(value=lecture["title"])
            e = ttk.Entry(b, textvariable=self.title_var, width=44, validate="key", validatecommand=win.vcmd(80))
            e.grid(row=2, column=0, columnspan=2, sticky="ew")
            self.err = ttk.Label(b, style="Err.TLabel", wraplength=420)
            self.err.grid(row=3, column=0, columnspan=2, sticky="w", pady=(8, 0))
            self.buttons(4, [("Cancel", self.cancel, False), ("Save", self.ok, True)])
            self.show(e)

        def ok(self):
            try:
                self.win.db.update_lecture(self.lecture["id"], title=self.title_var.get())
            except D.DbError as e:
                return self.fail(e, self.err)
            self.cancel()
            self.win.changed("Lecture renamed")


# --------------------------------------------------------------------------
# The window
# --------------------------------------------------------------------------

class CompanionWindow:
    TABS = ("lectures", "students")

    def __init__(self, root, app, data_dir=None, demo=False, poll=True):
        self.root, self.app, self.db = root, app, app.db
        self.data_dir, self.demo = data_dir, demo
        self.closing = False
        self.results = queue.Queue()
        self.device_worker = Worker(self.results)      # device scans and writes, one at a time, in order
        self.after_ids = {}
        self.busy_n = 0
        self.refreshing = 0
        self.state = None
        self.students, self.lectures, self.cards = [], [], []
        self.sent = None               # the lecture just sent, waiting for the cable to come out
        self.dismissed = set()
        self.waiting = None            # a student dialog waiting for a new card to be read
        self.sending = False
        self.title_touched = False
        self._quiet = False
        self.sel_lecture = None        # the lecture whose attendance is shown
        self.detail = None
        self.banner_sig = None
        self._vcmds = {}
        self.prefs = load_prefs(data_dir)

        root.title(APP_TITLE + (" (demo)" if demo else ""))
        w = min(1100, max(860, root.winfo_screenwidth() - 60))
        h = min(740, max(580, root.winfo_screenheight() - 100))
        root.geometry("%dx%d" % (w, h))
        root.minsize(860, 580)
        root.configure(bg=PAL["bg"])
        self._style()
        self._build()
        root.protocol("WM_DELETE_WINDOW", self.close)
        self._keys()
        self.reload()
        self.render()
        self._later("drain", 100, self._drain)
        if poll:
            self.poll()

    # ------------------------------------------------------------ plumbing
    def _later(self, name, ms, fn):
        if self.closing:
            return
        old = self.after_ids.pop(name, None)
        if old:
            self.root.after_cancel(old)

        def run():
            self.after_ids.pop(name, None)
            fn()
        self.after_ids[name] = self.root.after(ms, run)

    def _drain(self):
        try:
            while True:
                cb, value = self.results.get_nowait()
                if cb and not self.closing:
                    cb(value)
        except queue.Empty:
            pass
        finally:
            self._later("drain", 100, self._drain)

    def vcmd(self, n):
        """A validatecommand that keeps an entry to @n characters."""
        if n not in self._vcmds:
            self._vcmds[n] = (self.root.register(lambda p, n=n: len(p) <= n), "%P")
        return self._vcmds[n]

    def set_busy(self, on):
        self.busy_n = max(0, self.busy_n + (1 if on else -1))
        cur = "watch" if self.busy_n else ""
        for w in [self.root] + [c for c in self.root.winfo_children() if isinstance(c, tk.Toplevel)]:
            try:
                w.configure(cursor=cur)
            except tk.TclError:
                pass
        if on:
            self.root.update_idletasks()

    def _job(self, fn, done=None, failed=None, busy=True):
        if busy:
            self.set_busy(True)

        def ok(v):
            if busy:
                self.set_busy(False)
            if done:
                done(v)

        def bad(e):
            if busy:
                self.set_busy(False)
            (failed or self.show_error)(e)
        self.device_worker.submit(fn, ok, bad)

    def show_error(self, e, parent=None):
        messagebox.showerror(APP_TITLE, err_text(e), parent=parent or self.root)

    def toast(self, msg):
        self.msg_var.set(msg or "")
        if msg:
            self._later("toast", 6000, lambda: self.msg_var.set(""))

    def current_tab(self):
        try:
            return self.TABS[self.nb.index(self.nb.select())]
        except tk.TclError:
            return "lectures"

    def connected(self):
        return bool(self.state and self.state.get("connected"))

    # --------------------------------------------------------------- look
    def _style(self):
        r = self.root
        st = self.style = ttk.Style(r)
        if "clam" in st.theme_names():         # the one built-in theme that takes colours on every system
            st.theme_use("clam")
        for name in ("TkDefaultFont", "TkTextFont", "TkMenuFont", "TkHeadingFont", "TkCaptionFont", "TkIconFont"):
            try:
                tkfont.nametofont(name).configure(size=10)
            except tk.TclError:
                pass
        base = tkfont.nametofont("TkDefaultFont")
        fam = base.cget("family")
        self.f_title = tkfont.Font(family=fam, size=17, weight="bold")
        self.f_h1 = tkfont.Font(family=fam, size=15, weight="bold")
        self.f_h2 = tkfont.Font(family=fam, size=12, weight="bold")
        self.f_big = tkfont.Font(family=fam, size=24, weight="bold")
        self.f_bold = tkfont.Font(family=fam, size=10, weight="bold")
        self.f_small = tkfont.Font(family=fam, size=9)
        P = PAL

        st.configure(".", background=P["bg"], foreground=P["text"], bordercolor=P["line"])
        st.configure("TFrame", background=P["bg"])
        st.configure("Card.TFrame", background=P["card"])
        st.configure("TLabel", background=P["bg"], foreground=P["text"])
        for name, opts in (("H1", {"font": self.f_h1, "foreground": P["indigo_dark"]}),
                           ("H2", {"font": self.f_h2, "foreground": P["indigo_dark"]}),
                           ("Bold", {"font": self.f_bold}), ("Muted", {"foreground": P["muted"]}),
                           ("Err", {"foreground": P["red_dark"]}), ("Info", {"foreground": P["blue_dark"]})):
            st.configure(name + ".TLabel", **opts)
            st.configure("Card.%s.TLabel" % name, background=P["card"])
        st.configure("Card.TLabel", background=P["card"])

        st.configure("TButton", background=P["toolbar"], foreground=P["indigo_dark"], bordercolor=P["line"],
                     lightcolor=P["toolbar"], darkcolor=P["toolbar"], focuscolor=P["indigo"], padding=(12, 6))
        st.map("TButton", background=[("disabled", "#EEF0F6"), ("pressed", P["select"]), ("active", "#D9DEF5")],
               foreground=[("disabled", "#A3A8BA")])
        for name, c, dark in BUTTONS:
            c, dark = P[c], P[dark]
            st.configure(name + ".TButton", background=c, foreground="white", bordercolor=c, lightcolor=c,
                         darkcolor=c, focuscolor="white", font=self.f_bold, padding=(14, 7))
            st.map(name + ".TButton", background=[("disabled", "#C9CDDA"), ("pressed", dark), ("active", dark)],
                   bordercolor=[("disabled", "#C9CDDA"), ("active", dark)],
                   lightcolor=[("disabled", "#C9CDDA"), ("active", dark)],
                   darkcolor=[("disabled", "#C9CDDA"), ("active", dark)],
                   foreground=[("disabled", "#F5F6FA")])

        st.configure("TEntry", fieldbackground="white", bordercolor=P["line"], lightcolor=P["line"], padding=5)
        st.map("TEntry", bordercolor=[("focus", P["indigo"])], lightcolor=[("focus", P["indigo"])])
        st.configure("TCombobox", fieldbackground="white", bordercolor=P["line"], padding=4)

        st.configure("TNotebook", background=P["bg"], borderwidth=0, tabmargins=(12, 6, 12, 0))
        st.configure("TNotebook.Tab", background="#DDE2F5", foreground=P["indigo_dark"], font=self.f_bold,
                     padding=(22, 8), bordercolor=P["line"])
        st.map("TNotebook.Tab", background=[("selected", P["indigo"]), ("active", "#C9D2F5")],
               foreground=[("selected", "white")], expand=[("selected", (1, 1, 1, 0))])

        st.configure("Treeview", background="white", fieldbackground="white", foreground=P["text"],
                     bordercolor=P["line"], rowheight=base.metrics("linespace") + 10)
        st.configure("Treeview.Heading", background="#E3E7F7", foreground=P["indigo_dark"], font=self.f_bold,
                     bordercolor=P["line"], relief="flat", padding=(6, 5))
        st.map("Treeview.Heading", background=[("active", "#D3D9F3")])
        st.map("Treeview", background=[("selected", P["select"])], foreground=[("selected", P["text"])])
        st.configure("Vertical.TScrollbar", background="#D3D9F3", troughcolor=P["bg"], bordercolor=P["bg"],
                     arrowcolor=P["indigo"], lightcolor="#D3D9F3", darkcolor="#D3D9F3")
        st.configure("TPanedwindow", background=P["bg"])
        st.configure("Sash", sashthickness=8)

    # -------------------------------------------------------------- build
    def _build(self):
        r, P = self.root, PAL

        # The coloured header: name on the left, the device's state on the right.
        head = tk.Frame(r, bg=P["indigo"], padx=18, pady=12)
        head.pack(side="top", fill="x")
        tk.Label(head, text="\u25c9  " + APP_TITLE, bg=P["indigo"], fg="white", font=self.f_title).pack(side="left")
        self.pill = tk.Frame(head, padx=12, pady=5)
        self.pill.pack(side="right")
        self.pill_dot = tk.Label(self.pill, text="\u25cf", font=self.f_h2)
        self.pill_dot.pack(side="left")
        self.pill_title = tk.Label(self.pill, font=self.f_bold)
        self.pill_title.pack(side="left", padx=(4, 0))
        self.pill_detail = tk.Label(self.pill, font=self.f_small)
        self.pill_detail.pack(side="left", padx=(8, 0))

        # The device buttons.
        bar = tk.Frame(r, bg=P["toolbar"], padx=16, pady=9)
        bar.pack(side="top", fill="x")
        ttk.Button(bar, text="\u21bb  Read device", style="Blue.TButton",
                   command=lambda: self.poll(force=True)).pack(side="left")
        self.b_clock = ttk.Button(bar, text="Set device time", style="Teal.TButton", command=self.set_clock)
        self.b_clock.pack(side="left", padx=(8, 0))
        self.clock_lbl = tk.Label(bar, bg=P["toolbar"], fg=P["muted"], font=self.f_small)
        self.clock_lbl.pack(side="left", padx=(10, 0))
        self.b_clear = ttk.Button(bar, text="Clear device records", style="Red.TButton", command=self.clear_device)
        self.b_clear.pack(side="right")

        # bottom first, so the notebook takes what is left
        status = ttk.Frame(r, padding=(16, 4, 16, 6))
        status.pack(side="bottom", fill="x")
        self.msg_var = tk.StringVar()
        self.foot_var = tk.StringVar()
        ttk.Button(status, text="Quit", command=self.close).pack(side="right")
        ttk.Button(status, text="Back up\u2026", command=self.backup).pack(side="right", padx=(0, 6))
        ttk.Label(status, textvariable=self.foot_var, style="Muted.TLabel").pack(side="right", padx=(0, 12))
        ttk.Label(status, textvariable=self.msg_var, style="Info.TLabel").pack(side="left")

        self.banner_box = ttk.Frame(r, padding=(16, 8, 16, 0))
        self.banner_box.pack(side="top", fill="x")

        self.nb = ttk.Notebook(r, padding=(10, 4, 10, 4))
        self.nb.pack(side="top", fill="both", expand=True)
        self.tabs = {}
        for key, text in (("lectures", "Lectures"), ("students", "Students")):
            f = ttk.Frame(self.nb, padding=14)
            self.nb.add(f, text=text, underline=0)
            self.tabs[key] = f
        self.nb.enable_traversal()
        self._build_lectures(self.tabs["lectures"])
        self._build_students(self.tabs["students"])
        self.nb.bind("<<NotebookTabChanged>>", self._tab_changed)

    def _keys(self):
        r = self.root
        for i in range(len(self.TABS)):
            r.bind("<Control-Key-%d>" % (i + 1), lambda e, i=i: self.nb.select(i))
        r.bind("<F5>", lambda e: self.poll(force=True))
        r.bind("<Control-f>", self.focus_search)
        r.bind("<Control-q>", lambda e: self.close())

    def focus_search(self, e=None):
        target = self.e_st_q if self.current_tab() == "students" else self.e_lec_q
        target.focus_set()
        target.select_range(0, "end")
        return "break"

    # ---- lectures tab
    def _build_lectures(self, tab):
        P = PAL
        tab.columnconfigure(0, weight=1)
        tab.rowconfigure(1, weight=1)

        start = ttk.Frame(tab, style="Card.TFrame", padding=(16, 12))
        start.grid(row=0, column=0, sticky="ew", pady=(0, 12))
        start.columnconfigure(0, weight=1)
        ttk.Label(start, text="Start a lecture", style="Card.H2.TLabel").grid(row=0, column=0, columnspan=3, sticky="w")
        ttk.Label(start, text="Lecture name", style="Card.TLabel").grid(row=1, column=0, sticky="w", pady=(8, 2))
        self.f_title = tk.StringVar()
        self.e_title = ttk.Entry(start, textvariable=self.f_title, width=40, validate="key",
                                 validatecommand=self.vcmd(D.LECTURE_BYTES))
        self.e_title.grid(row=2, column=0, sticky="ew")
        self.e_title.bind("<Return>", lambda e: self.start_lecture())
        self.b_send = ttk.Button(start, text="\u25b6  Start lecture", style="Green.TButton", command=self.start_lecture)
        self.b_send.grid(row=2, column=1, sticky="w", padx=(10, 0))
        self.send_why = ttk.Label(start, style="Card.Muted.TLabel")
        self.send_why.grid(row=2, column=2, sticky="w", padx=(10, 0))
        wrapping(ttk.Label(start, style="Card.Muted.TLabel", text=(
            "Saves the device's taps here, sets its clock and starts the lecture. No computer at hand? Hold the "
            "device's button until it buzzes (2 seconds) and let go: the next lecture starts, and appears here the "
            "next time you plug the device in.")), start, margin=40).grid(row=3, column=0, columnspan=3, sticky="w",
                                                                           pady=(8, 0))
        self.f_title.trace_add("write", self._title_changed)

        pw = ttk.PanedWindow(tab, orient="horizontal")
        pw.grid(row=1, column=0, sticky="nsew")
        self._sash_set = False

        def first_size(e):
            if not self._sash_set and e.width > 100:
                self._sash_set = True
                pw.sashpos(0, max(320, int(e.width * 0.42)))
        pw.bind("<Configure>", first_size, add="+")

        left = ttk.Frame(pw, padding=(0, 0, 10, 0))
        lh = ttk.Frame(left)
        lh.pack(fill="x", pady=(0, 6))
        ttk.Label(lh, text="Lectures", style="H2.TLabel").pack(side="left")
        self.lec_count = ttk.Label(lh, style="Muted.TLabel")
        self.lec_count.pack(side="left", padx=(8, 0))
        self.lec_table = Table(left, [col("title", "Lecture", 170), col("date", "Date", 100, "w", False),
                                      col("time", "Start", 60, "center", False),
                                      col("present", "Present", 70, "center", False)], height=14, sortable=False)
        self.lec_table.pack(fill="both", expand=True)
        self.lec_table.tree.bind("<<TreeviewSelect>>", self._lecture_picked)
        pw.add(left, weight=2)

        right = ttk.Frame(pw, style="Card.TFrame", padding=16)
        pw.add(right, weight=3)
        self.d_empty = ttk.Label(right, style="Card.Muted.TLabel", text="Pick a lecture to see who came.")
        f = self.d_lec = ttk.Frame(right, style="Card.TFrame")

        top = ttk.Frame(f, style="Card.TFrame")
        top.pack(fill="x")
        tl = ttk.Frame(top, style="Card.TFrame")
        tl.pack(side="left", fill="x", expand=True)
        self.l_title = ttk.Label(tl, style="Card.H1.TLabel")
        self.l_title.pack(anchor="w")
        self.l_sub = ttk.Label(tl, style="Card.Muted.TLabel")
        self.l_sub.pack(anchor="w", pady=(2, 0))
        badge = tk.Frame(top, bg=P["green_soft"], padx=16, pady=4)
        badge.pack(side="right")
        self.l_num = tk.Label(badge, bg=P["green_soft"], fg=P["green_dark"], font=self.f_big)
        self.l_num.pack()
        tk.Label(badge, text="present", bg=P["green_soft"], fg=P["green_dark"], font=self.f_small).pack()

        qr = ttk.Frame(f, style="Card.TFrame")
        qr.pack(fill="x", pady=(12, 8))
        ttk.Label(qr, text="Search", style="Card.TLabel").pack(side="left")
        self.l_q = tk.StringVar()
        self.e_lec_q = ttk.Entry(qr, textvariable=self.l_q, width=28)
        self.e_lec_q.pack(side="left", padx=(8, 0))
        self.l_q.trace_add("write", lambda *a: self.draw_detail())

        br = ttk.Frame(f, style="Card.TFrame")
        br.pack(side="bottom", fill="x", pady=(10, 0))
        ttk.Button(br, text="Rename\u2026", command=self.rename_lecture).pack(side="left")
        self.b_end = ttk.Button(br, text="End lecture", command=self.end_lecture)
        self.b_end.pack(side="left", padx=(6, 0))
        ttk.Button(br, text="Delete", style="Red.TButton", command=self.delete_lecture).pack(side="left", padx=(6, 0))
        ttk.Button(br, text="Save list (CSV)\u2026", style="Indigo.TButton", command=self.save_lecture_csv).pack(
            side="right")

        self.unreg = tk.Frame(f, bg=P["amber_soft"], padx=12, pady=6, highlightthickness=1,
                              highlightbackground=P["orange"])
        self.unreg_lbl = tk.Label(self.unreg, bg=P["amber_soft"], fg=P["amber_text"], font=self.f_bold,
                                  justify="left", anchor="w")
        self.unreg_lbl.pack(side="left", fill="x", expand=True)
        ttk.Button(self.unreg, text="Register\u2026", style="Orange.TButton", command=self.register_from_lecture).pack(
            side="right")

        self.l_table = Table(f, [col("no", "Index No", 110, "w", False), col("name", "Name", 230),
                                 col("time", "Time", 80, "center", False)], height=8)
        self.l_table.pack(fill="both", expand=True)
        self.l_table.on_activate(lambda iid: StudentDialog(self, int(iid)))

    # ---- students tab
    def _build_students(self, tab):
        P = PAL
        tab.columnconfigure(0, weight=1)
        tab.rowconfigure(2, weight=1)

        nc = self.newcards = tk.Frame(tab, bg=P["amber_soft"], padx=14, pady=10, highlightthickness=1,
                                      highlightbackground=P["orange"])
        self.nc_title = tk.Label(nc, bg=P["amber_soft"], fg=P["amber_text"], font=self.f_h2)
        self.nc_title.pack(anchor="w")
        tk.Label(nc, bg=P["amber_soft"], fg=P["amber_text"], text="These cards were tapped but belong to nobody yet. "
                 "Register each one with an index number and a name.").pack(anchor="w", pady=(2, 6))
        row = tk.Frame(nc, bg=P["amber_soft"])
        row.pack(fill="x")
        self.nc_table = Table(row, [col("card", "Card", 120), col("taps", "Tapped", 90, "center", False),
                                    col("last", "Last tap", 160)], height=3)
        self.nc_table.pack(side="left", fill="x", expand=True)
        self.nc_table.on_activate(lambda iid: self.register(int(iid)))
        ttk.Button(row, text="Register\u2026", style="Orange.TButton",
                   command=lambda: self.nc_table.selected() and self.register(int(self.nc_table.selected()))).pack(
            side="left", padx=(10, 0), anchor="n")
        nc.grid(row=0, column=0, sticky="ew", pady=(0, 12))

        head = ttk.Frame(tab)
        head.grid(row=1, column=0, sticky="ew", pady=(0, 8))
        hl = ttk.Frame(head)
        hl.pack(side="left")
        tr = ttk.Frame(hl)
        tr.pack(anchor="w")
        ttk.Label(tr, text="Students", style="H1.TLabel").pack(side="left")
        self.st_count = ttk.Label(tr, style="Muted.TLabel")
        self.st_count.pack(side="left", padx=(10, 0), anchor="s", pady=(0, 3))
        self.st_devline = ttk.Label(hl, style="Info.TLabel")
        self.st_devline.pack(anchor="w")
        sr = ttk.Frame(hl)
        sr.pack(anchor="w", pady=(8, 0))
        ttk.Label(sr, text="Search").pack(side="left")
        self.st_q = tk.StringVar()
        self.e_st_q = ttk.Entry(sr, textvariable=self.st_q, width=32)
        self.e_st_q.pack(side="left", padx=(8, 0))
        self.st_q.trace_add("write", lambda *a: self.draw_students())
        hb = ttk.Frame(head)
        hb.pack(side="right", anchor="n")
        ttk.Button(hb, text="+  Add student", style="Green.TButton",
                   command=lambda: StudentDialog(self, None, {})).pack(side="left")
        ttk.Button(hb, text="Import list\u2026", command=lambda: ImportDialog(self)).pack(side="left", padx=(6, 0))
        self.b_send_cards = ttk.Button(hb, text="Send cards to device", style="Purple.TButton", command=self.send_cards)
        self.b_send_cards.pack(side="left", padx=(6, 0))

        self.st_table = Table(tab, [col("no", "Index No", 120, "w", False), col("name", "Name", 260),
                                    col("card", "Card", 120, "w", False), col("last", "Last tap", 160, "w", False)],
                              height=10)
        self.st_table.grid(row=2, column=0, sticky="nsew")
        self.st_table.on_activate(lambda iid: StudentDialog(self, int(iid)))
        self.st_table.tree.bind("<Delete>", lambda e: self.delete_student())

        ar = ttk.Frame(tab)
        ar.grid(row=3, column=0, sticky="ew", pady=(8, 0))
        ttk.Button(ar, text="Edit\u2026", command=lambda: self.st_table.selected() and StudentDialog(
            self, int(self.st_table.selected()))).pack(side="left")
        ttk.Button(ar, text="Delete", style="Red.TButton", command=self.delete_student).pack(side="left", padx=(6, 0))
        wrapping(ttk.Label(ar, style="Muted.TLabel", text=(
            "New student: tap their card on the device, plug the device in, then Register the card above and press "
            "Send cards to device.")), ar, margin=260).pack(side="left", padx=(16, 0))

    # ------------------------------------------------------------- device
    def poll(self, force=False):
        """Look at the device on the worker thread; again every POLL_MS."""
        if self.closing:
            return
        if force or not self.refreshing:
            self.refreshing += 1
            if force:
                self.toast("Reading the device\u2026")
            self._job(lambda: self.app.refresh(force), self._got_state, self._poll_failed, busy=force)
        self._later("poll", POLL_MS, self.poll)

    def _poll_failed(self, e):
        self.refreshing = max(0, self.refreshing - 1)
        self.toast("Could not read the device: %s" % err_text(e))

    def _got_state(self, st):
        self.refreshing = max(0, self.refreshing - 1)
        prev, self.state = self.state, st
        changed = prev is None or prev["connected"] != st["connected"] or prev["sync"]["seq"] != st["sync"]["seq"]
        if prev and st["sync"]["seq"] != prev["sync"]["seq"] and (st["sync"]["new"] or st["sync"].get("lectures")):
            bits = [plural(st["sync"]["new"], "new tap")]
            if st["sync"].get("lectures"):
                bits.append(plural(st["sync"]["lectures"], "new lecture"))
            self.toast(" and ".join(bits) + " read from the device")
        elif self.msg_var.get() == "Reading the device\u2026":
            self.toast("The device was read at %s" % st["sync"]["at"] if st["connected"] else "The device is not connected.")
        if st["connected"]:
            d = st.get("device")
            s = self.sent
            if s and d and d["has_lecture"] and d["module"] == s["module"] and d["lecture"] == s["title"] and not d["pending"]:
                s["done"] = True
        if changed:
            self.reload()
            self._check_waiting()
            self.render()
        else:
            self.render_chrome()

    def _check_waiting(self):
        w = self.waiting
        if not w:
            return
        dlg = w["dialog"]
        try:
            alive = bool(dlg.winfo_exists())
        except tk.TclError:
            alive = False
        if not alive:
            self.waiting = None
            return
        fresh = [c for c in self.cards if c["card_id"] not in w["known"]]
        if fresh:
            self.waiting = None
            dlg.card_read(fresh[0]["card_id"])
            self.toast("Card %s read from the device" % card10(fresh[0]["card_id"]))

    def set_clock(self):
        if not self.connected():
            return

        def done(ejected):
            self.sent = None
            self.toast("Device time set to this computer's. The device has it now." if ejected else
                       "Time sent. Eject the drive or press the device's button to apply it.")
            self.poll()
        self._job(lambda: (self.app.set_clock(), self.app.eject())[1], done)

    def clear_device(self):
        if not self.connected():
            return
        if not messagebox.askyesno(APP_TITLE, "Clear the records in the device's memory?\n\nEvery tap and lecture is "
                                   "first copied to this computer; only then does the device erase them. The students "
                                   "and everything already on this computer are kept.", parent=self.root,
                                   icon="warning"):
            return

        def done(r):
            self.sent = None
            self.changed("All records copied here and erased from the device." if r.get("ejected") else
                         "Records copied here. Eject the drive or press the device's button: it then erases them.",
                         poll=True)
        self._job(self.app.clear_device, done)

    def send_cards(self):
        def done(n):
            self.sent = None
            self.toast(plural(n, "card") + " sent. Eject the drive or press the device's button to apply.")
            self.render_banners()
            self.poll()
        self._job(self.app.send_cards, done)

    def eject(self):
        def done(ok):
            self.toast("Ejected. The device applies the changes and starts taking attendance." if ok else
                       "Could not eject: use Eject in the file manager, or press the button on the device once.")
            self.poll()
        self._job(self.app.eject, done)

    # --------------------------------------------------------------- data
    def reload(self):
        """Read everything the screens show from the database."""
        db = self.db
        try:
            self.students = db.list_students()
            self.lectures = db.list_lectures()
            self.cards = db.unregistered_cards()
        except Exception as e:
            self.toast("Could not read the database: %s" % e)
        self.refresh_selected()

    def refresh_selected(self):
        if self.sel_lecture is None and self.lectures:
            cur = self.db.current_lecture()
            self.sel_lecture = cur["id"] if cur else self.lectures[0]["id"]
        if self.sel_lecture is not None:
            try:
                self.detail = self.db.lecture_attendance(self.sel_lecture)
            except D.DbError:
                self.sel_lecture, self.detail = None, None
        else:
            self.detail = None

    def changed(self, msg, poll=False):
        """After a change made here: say so, read the database again, redraw."""
        if msg:
            self.toast(msg)
        self.reload()
        self.render()
        if poll:
            self.poll()

    def _tab_changed(self, e=None):
        if self.closing:
            return
        self.reload()
        self.render()

    # ------------------------------------------------------------- render
    def render(self):
        self.render_chrome()
        getattr(self, "render_" + self.current_tab())()

    def render_chrome(self):
        st = self.state
        kind, title, detail = device_summary(st, self.demo)
        bg, fg = COLORS["ok" if kind == "ok" else ("warn" if kind == "wait" else "off")]
        self.pill.configure(bg=bg)
        self.pill_dot.configure(bg=bg, fg=DOTS[kind])
        self.pill_title.configure(bg=bg, fg=fg, text=title)
        self.pill_detail.configure(bg=bg, fg=fg, text=detail)
        on = ["!disabled"] if self.connected() else ["disabled"]
        for b in (self.b_clock, self.b_clear, self.b_send_cards):
            b.state(on)
        self.clock_lbl.configure(text=lecture_clock_text(st))
        self.st_devline.configure(text=cards_line(st))
        c = st["counts"] if st else self.db.counts()
        self.foot_var.set(("Demo data: nothing here is real. " if self.demo else "") + "%s \u00b7 %s \u00b7 %s"
                          % (plural(c["students"], "student"), plural(c["lectures"], "lecture"), plural(c["taps"], "tap")))
        self.render_banners()
        self._lecture_controls()

    def render_banners(self):
        items = banner_list(self.state, self.sent, self.current_tab(), self.dismissed)
        sig = tuple((b["key"], b["title"], b["text"]) for b in items)
        if sig == self.banner_sig:
            return
        self.banner_sig = sig
        for w in self.banner_box.winfo_children():
            w.destroy()
        for b in items:
            bg, fg = COLORS[b["kind"]]
            f = tk.Frame(self.banner_box, bg=bg, padx=12, pady=7, highlightthickness=1, highlightbackground=fg)
            f.pack(fill="x", pady=(0, 6))
            if b["action"] == "send-cards":
                ttk.Button(f, text="Send cards to device", style="Purple.TButton", command=self.send_cards).pack(
                    side="right", padx=(10, 0))
            elif b["action"] == "eject":
                ttk.Button(f, text="Eject now", style="Blue.TButton", command=self.eject).pack(side="right", padx=(10, 0))
            else:
                tk.Button(f, text="\u2715", relief="flat", bg=bg, fg=fg, activebackground=bg, bd=0, cursor="hand2",
                          command=lambda k=b["key"]: self.dismiss(k)).pack(side="right", padx=(10, 0))
            tk.Label(f, text=b["title"], bg=bg, fg=fg, font=self.f_bold).pack(side="left", anchor="n")
            wrapping(tk.Label(f, text=b["text"], bg=bg, fg=fg, justify="left", anchor="w"), f, margin=420).pack(
                side="left", padx=(8, 0), fill="x", expand=True, anchor="w")

    def dismiss(self, key):
        self.dismissed.add(key)
        self.render_banners()

    # ---- lectures
    def render_lectures(self):
        if not self.title_touched:
            self._quiet = True
            self.f_title.set(suggest_title(self.lectures, None, self.sent))
            self._quiet = False
        rows = []
        for l in self.lectures:                 # newest first
            c = l["counts"]
            rows.append((str(l["id"]), (("\u25cf " if l["running"] else "") + l["title"], l["date"],
                                        l["start_text"][11:16], c["present_enrolled"] + c["present_other"]),
                         ("running",) if l["running"] else ()))
        self.lec_table.fill(rows, "No lectures yet. Start one above, or hold the device's button for 2 seconds.")
        self.lec_count.configure(text=plural(len(self.lectures), "lecture") if self.lectures else "")
        if self.sel_lecture is not None:
            self.lec_table.select(str(self.sel_lecture))
        self.render_detail()
        self._lecture_controls()

    def _lecture_picked(self, e=None):
        iid = self.lec_table.selected()
        if iid and int(iid) != self.sel_lecture:
            self.sel_lecture = int(iid)
            self.l_q.set("")
            self.refresh_selected()
            self.render_detail()

    def render_detail(self):
        d = self.detail
        if not d:
            self.d_lec.pack_forget()
            self.d_empty.pack(anchor="w")
            return
        self.d_empty.pack_forget()
        self.d_lec.pack(fill="both", expand=True)
        l = d["lecture"]
        self.l_title.configure(text=l["title"])
        when = "still running" if l["running"] else "until " + l["end_text"][11:16]
        self.l_sub.configure(text="%s \u00b7 started %s \u00b7 %s" % (nice_date(l["start_ts"]), l["start_text"][11:16], when))
        self.l_num.configure(text=str(len(present_students(d))))
        un = d["unregistered"]
        if un:
            self.unreg_lbl.configure(text="%s tapped in this lecture but %s not registered yet." % (
                plural(len(un), "card"), "is" if len(un) == 1 else "are"))
            self.unreg.pack(side="bottom", fill="x", pady=(8, 0), before=self.l_table)
        else:
            self.unreg.pack_forget()
        if l["running"]:
            self.b_end.pack(side="left", padx=(6, 0), after=self.b_end.master.winfo_children()[0])
        else:
            self.b_end.pack_forget()
        self.draw_detail()

    def draw_detail(self):
        qs = self.l_q.get()
        rows = [(str(s["card_id"]), (s["student_no"] or "\u2014", s["name"], s["time"][:5]), ())
                for s in present_students(self.detail)
                if matches(qs, [s["name"], s["student_no"], s["card_id"], card10(s["card_id"])])]
        self.l_table.fill(rows, "Nobody has tapped in this lecture yet." if not present_students(self.detail)
                          else "Nobody matches that search.")

    def _title_changed(self, *a):
        if not self._quiet:
            self.title_touched = True
            self._lecture_controls()

    def _lecture_controls(self):
        t = self.f_title.get().strip()
        if self.sending:
            why = "Sending\u2026"
        elif not self.connected():
            why = "Plug in the device to start a lecture from here."
        elif not t:
            why = "Give the lecture a name."
        else:
            why = ""
        self.b_send.state(["disabled"] if why else ["!disabled"])
        self.send_why.configure(text=why)

    def start_lecture(self):
        if self.b_send.instate(["disabled"]):
            return
        module, title = lecture_module(self.lectures), self.f_title.get()
        self.sending = True
        self._lecture_controls()

        def done(r):
            self.sending = False
            self.sent = {"module": r["lecture"]["module_code"], "title": r["lecture"]["title"], "done": False,
                         "ejected": r.get("ejected", False), "cleared": r.get("cleared", False)}
            self.dismissed = set()
            self.sel_lecture = r["lecture"]["id"]
            self.title_touched = False
            self.changed("Sent and ejected. The lecture starts on the device now." if r.get("ejected") else
                         "Sent. Now eject the drive or press the button on the device.", poll=True)

        def failed(e):
            self.sending = False
            self.show_error(e)
            self.render()
        self._job(lambda: self.app.start_lecture(module, title, True), done, failed)

    def _lecture(self):
        return self.detail["lecture"] if self.detail else None

    def rename_lecture(self):
        if self._lecture():
            RenameDialog(self, self._lecture())

    def end_lecture(self):
        l = self._lecture()
        if not l:
            return
        try:
            self.db.end_lecture(l["id"])
        except D.DbError as e:
            return self.show_error(e)
        self.changed("Lecture ended")

    def delete_lecture(self):
        l = self._lecture()
        if not l:
            return
        if not messagebox.askyesno(APP_TITLE, "Delete the lecture \u201c%s\u201d? The taps stay; they just stop "
                                   "counting as this lecture." % l["title"], parent=self.root, icon="warning"):
            return
        try:
            self.db.delete_lecture(l["id"])
        except D.DbError as e:
            return self.show_error(e)
        self.sel_lecture, self.detail = None, None
        self.changed("Lecture deleted")

    def register_from_lecture(self):
        if self.detail and self.detail["unregistered"]:
            self.register(self.detail["unregistered"][0]["card_id"])

    def save_lecture_csv(self):
        l = self._lecture()
        if not l:
            return
        data = lecture_csv(self.detail).encode("utf-8-sig")      # the BOM lets Excel read the names right
        self._save_file((data, "text/csv", "attendance-%s-%s.csv" % (A._slug(l["title"]), l["date"])))

    # ---- students
    def render_students(self):
        if self.cards:
            self.nc_title.configure(text="New cards to register (%d)" % len(self.cards))
            self.nc_table.fill([(str(c["card_id"]), (card10(c["card_id"]), plural(c["taps"], "time"), c["last"]), ())
                                for c in self.cards])
            self.newcards.grid()
        else:
            self.newcards.grid_remove()
        self.draw_students()

    def draw_students(self):
        qs = self.st_q.get()
        rows = [s for s in self.students
                if matches(qs, [s["name"], s["student_no"], s["card_id"], card10(s["card_id"])])]
        n = len(self.students)
        self.st_count.configure(text=plural(n, "student") if len(rows) == n else "%d of %s" % (len(rows), plural(n, "student")))
        self.st_table.fill([(str(s["card_id"]), (s["student_no"] or "\u2014", s["name"], card10(s["card_id"]),
                                                 s["last_tap_text"] or "\u2014"), ()) for s in rows],
                           "No students yet. Add one, import a class list, or tap a card on the device and register "
                           "it when it appears." if not n else "Nobody matches that search.")

    def register(self, card_id):
        StudentDialog(self, None, {"card_id": card_id})

    def delete_student(self):
        iid = self.st_table.selected()
        if not iid:
            return
        if not messagebox.askyesno(APP_TITLE, "Delete this student? Their taps stay, and show as an unregistered "
                                   "card.", parent=self.root, icon="warning"):
            return
        try:
            self.db.delete_student(int(iid))
        except D.DbError as e:
            return self.show_error(e)
        self.changed("Student deleted")

    # ------------------------------------------------------------- files
    def _save_file(self, ex):
        data, ctype, name = ex
        ext = os.path.splitext(name)[1].lower()
        types = {".csv": [("CSV files", "*.csv")], ".db": [("Database files", "*.db")]}.get(ext, []) + [("All files", "*")]
        folder = self.prefs.get("save_dir")
        path = filedialog.asksaveasfilename(
            parent=self.root, title="Save " + name, initialfile=name, defaultextension=ext, filetypes=types,
            initialdir=folder if folder and os.path.isdir(folder) else os.path.expanduser("~"))
        if not path:
            return
        try:
            with open(path, "wb") as f:
                f.write(data)
        except OSError as e:
            return self.show_error(e)
        self.prefs["save_dir"] = os.path.dirname(path)
        save_prefs(self.data_dir, self.prefs)
        self.toast("Saved %s" % path)

    def backup(self):
        try:
            ex = A.backup_export(self.db)
        except (A.ApiError, D.DbError, OSError) as e:
            return self.show_error(e)
        self._save_file(ex)

    # -------------------------------------------------------------- close
    def close(self):
        if self.closing:
            return
        self.closing = True
        for aid in list(self.after_ids.values()):
            try:
                self.root.after_cancel(aid)
            except tk.TclError:
                pass
        self.after_ids.clear()
        self.device_worker.stop(5.0)
        save_prefs(self.data_dir, self.prefs)
        try:
            self.root.destroy()
        except tk.TclError:
            pass


# --------------------------------------------------------------------------
# Starting up
# --------------------------------------------------------------------------

def _windows_dpi():
    """Sharp text on high-DPI Windows screens."""
    if os.name != "nt":
        return
    try:
        import ctypes
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        pass


def main(argv=None):
    ap = argparse.ArgumentParser(description="Attendance Logger companion app (desktop window)")
    ap.add_argument("--device-dir", help="use this folder as the device instead of searching drives")
    ap.add_argument("--data-dir", help="where the database lives (default: %s)" % A.default_data_dir())
    ap.add_argument("--demo", action="store_true", help="use made-up data in temporary folders; touches nothing real")
    args = ap.parse_args(argv)

    if tk is None:
        print("This Python has no Tk, which the window needs. Install Python from https://www.python.org/downloads/ "
              "(on Linux: the python3-tk package), or use the browser version: python attendance_app.py",
              file=sys.stderr)
        return 1

    forced, data_dir = args.device_dir, args.data_dir or A.default_data_dir()
    scratch = []
    if args.demo:
        data_dir = tempfile.mkdtemp(prefix="attendance-demo-data-")
        forced = tempfile.mkdtemp(prefix="attendance-demo-device-")
        scratch = [data_dir, forced]
        A.make_demo(data_dir, forced)
        print("Demo data in %s and %s (nothing real is touched)" % (data_dir, forced), flush=True)

    _windows_dpi()
    try:
        root = tk.Tk()
    except tk.TclError as e:
        print("Could not open a window (%s). The browser version still works: python attendance_app.py" % e,
              file=sys.stderr)
        return 1
    root.withdraw()

    lock = db = None
    try:
        try:
            os.makedirs(data_dir, exist_ok=True)
        except OSError as e:
            messagebox.showerror(APP_TITLE, "Could not use the folder %s: %s" % (data_dir, e))
            return 1
        lock = InstanceLock(data_dir)
        if not lock.acquire():
            lock = None
            messagebox.showinfo(APP_TITLE, "The Attendance Logger is already open for %s. Use that window (look for it "
                                "on the taskbar)." % data_dir)
            return 0
        if not args.demo and not forced and A.already_running(A.DEFAULT_PORT):
            if not messagebox.askyesno(APP_TITLE, "The browser version of the Attendance Logger is already running. "
                                       "Close it (Quit app, at the bottom of its page) before using this window.\n\n"
                                       "Open this window anyway?", icon="warning"):
                return 0
        try:
            db = D.Database(os.path.join(data_dir, "attendance.db"))
        except Exception as e:
            messagebox.showerror(APP_TITLE, "Could not open the database: %s" % e)
            return 1
        if not args.demo:
            try:
                A.rotate_backup(db, data_dir)
            except OSError:
                pass
        print("Database: %s" % db.path, flush=True)
        win = CompanionWindow(root, A.App(db, forced, data_dir), data_dir, demo=args.demo)
        import signal
        for name in ("SIGINT", "SIGTERM"):           # Ctrl+C in the terminal, or being told to stop: close tidily
            try:
                signal.signal(getattr(signal, name), lambda *a: root.after(0, win.close))
            except (ValueError, OSError, AttributeError):
                pass
        root.deiconify()
        root.mainloop()
        win.close()
        return 0
    finally:
        if db is not None:
            db.close()
        if lock is not None:
            lock.release()
        try:
            root.destroy()
        except tk.TclError:
            pass
        for d in scratch:
            shutil.rmtree(d, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

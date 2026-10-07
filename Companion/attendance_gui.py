#!/usr/bin/env python3
"""
Attendance Logger companion app: the desktop window.

The same app as attendance_app.py, in a window of its own instead of a browser
tab. Nothing listens on the network: the window calls the same Python that the
browser version's web routes call. It needs only Python 3.8 or newer with Tk
(included in the python.org installers for Windows and macOS; on Linux it is
the python3-tk package).

    python attendance_gui.py                     normal use
    python attendance_gui.py --demo              try it with made-up data, no device
    python attendance_gui.py --data-dir D:\\Attendance    keep the database elsewhere
    python attendance_gui.py --device-dir FOLDER  use a folder as the device

Device scanning and anything written to the device run on a worker thread; the
results come back to the window through a queue, so the window never freezes
while Windows wakes up a slow drive.
"""
import argparse
import json
import os
import pathlib
import queue
import re
import shutil
import sys
import tempfile
import threading
import time
import webbrowser

try:
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
    from tkinter import font as tkfont
except ImportError:          # a Python without Tk: main() says what to do
    tk = None

import attendance_app as A
import attendance_db as D
import report_pages as R

APP_TITLE = "Attendance Logger"
POLL_MS = 2000
LOCK_NAME = "companion.lock"
PREFS_NAME = "companion-gui.json"
DRIFT_LIMIT = 120              # seconds before the clock is called wrong, as in the browser version

COLORS = {                     # banner and status colours: background, text
    "ok": ("#e6f4ea", "#1e4620"),
    "info": ("#e8f0fe", "#123a7a"),
    "warn": ("#fef7e0", "#5c3d00"),
    "bad": ("#fce8e6", "#8a1f17"),
    "off": ("#eeeeee", "#333333"),
}
DOTS = {"ok": "#1e8e3e", "wait": "#e37400", "off": "#888888"}


# --------------------------------------------------------------------------
# Plain helpers (no Tk; the tests use them directly)
# --------------------------------------------------------------------------

def plural(n, one, many=None):
    return "%d %s" % (n, one if n == 1 else (many or one + "s"))


def card10(n):
    return D.fmt_card(int(n))


def matches(qs, parts):
    """Case-insensitive substring search over @parts, as the browser version's search boxes do."""
    if not qs:
        return True
    qs = qs.lower()
    return any(qs in str("" if p is None else p).lower() for p in parts)


def suggest_title(lectures, module, sent=None):
    """The next lecture name for @module: the newest one's number plus one."""
    last = ""
    for l in lectures:                       # newest first
        if not module or l["module_code"] == module:
            last = l["title"]
            break
    if sent and sent.get("module") == module:
        last = sent["title"]
    m = re.match(r"^(.*?)(\d+)\s*$", last)
    if m:
        return m.group(1) + str(int(m.group(2)) + 1)
    return last + " 2" if last else "Lecture 1"


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
        bits = []
        if st.get("path"):
            bits.append(st["path"])
        bits.append(plural(d.get("records", 0), "tap") + " on the device")
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


def banner_list(st, sent, tab, dismissed=()):
    """The banners under the header, as the browser version shows them.
    Each is {key, kind, title, text, action}; action 'send-cards' carries a button and cannot be dismissed."""
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
                "Eject the drive, or press the button on the device, to apply them.")
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
        if drift is not None and abs(drift) > DRIFT_LIMIT and tab != "lecture":
            add("drift", "warn", "The device clock is %d minutes %s this computer."
                % (drift_minutes(drift), "ahead of" if drift > 0 else "behind"), "Starting a lecture sets it.")
    if sent and sent.get("done"):
        add("applied:" + sent["title"], "ok", "Lecture started on the device \u2713",
            "%s \u00b7 %s. Students can tap their cards now." % (sent["module"], sent["title"]))
    elif sent and sent.get("ejected"):
        add("await:" + sent["title"], "ok", "Sent to the device and ejected.",
            "%s \u00b7 %s. Green light and two buzzes on the device means the lecture started: students can tap "
            "their cards now, with the cable still in. Red light and three buzzes means it was refused. %s"
            % (sent["module"], sent["title"],
               "The device's old records are saved here and were deleted from it." if sent.get("cleared") else
               "The device kept its old records."))
    elif sent:
        add("await:" + sent["title"], "info", "Sent to the device.",
            "Now eject the drive, or press the button on the device once. The device then checks the file: green "
            "light and two buzzes means started, red light and three buzzes means it was refused.")
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
        """A Treeview with scroll bars; clicking a column heading sorts by it (again: the other way)."""

        def __init__(self, parent, columns=(), height=10, hscroll=False, sortable=True, selectmode="browse"):
            ttk.Frame.__init__(self, parent)
            self.tree = ttk.Treeview(self, show="headings", height=height, selectmode=selectmode)
            ys = ttk.Scrollbar(self, orient="vertical", command=self.tree.yview)
            self.tree.configure(yscrollcommand=ys.set)
            self.tree.grid(row=0, column=0, sticky="nsew")
            ys.grid(row=0, column=1, sticky="ns")
            if hscroll:
                xs = ttk.Scrollbar(self, orient="horizontal", command=self.tree.xview)
                self.tree.configure(xscrollcommand=xs.set)
                xs.grid(row=1, column=0, sticky="ew")
            self.columnconfigure(0, weight=1)
            self.rowconfigure(0, weight=1)
            self.sortable = sortable
            self.sort_key, self.sort_desc = None, False
            self.cols, self.rows = [], []
            self.empty = ttk.Label(self, style="Muted.TLabel", justify="center", wraplength=420)
            if columns:
                self.set_columns(columns)

        def set_columns(self, columns):
            columns = list(columns)
            if [c[:3] for c in columns] == [c[:3] for c in self.cols]:
                return
            self.cols = columns
            keys = [c[0] for c in columns]
            self.tree.configure(columns=keys, displaycolumns=keys)
            for key, head, width, anchor, stretch in columns:
                self.tree.heading(key, text=head, anchor=anchor,
                                  command=(lambda k=key: self.sort_by(k)) if self.sortable else "")
                self.tree.column(key, width=width, minwidth=30, anchor=anchor, stretch=stretch)
            if self.sort_key not in keys:
                self.sort_key = None
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
                s = str(v).strip().rstrip("%")
                try:
                    return (0, float(s), "")
                except ValueError:
                    return (1, 0.0, str(v).lower())
            return sorted(rows, key=k, reverse=self.sort_desc)

        def fill(self, rows, empty_text=""):
            """rows: [(iid, values, tags)]. The selection is kept where its rows still exist."""
            self.rows = list(rows)
            t = self.tree
            sel, focus = t.selection(), t.focus()
            t.delete(*t.get_children())
            for iid, values, tags in self._sorted(self.rows):
                t.insert("", "end", iid=iid, values=[("" if v is None else v) for v in values], tags=tags)
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
            self.transient(win.root)
            self.body = ttk.Frame(self, padding=16)
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
            f.grid(row=row, column=0, columnspan=2, sticky="ew", pady=(14, 0))
            if danger:
                ttk.Button(f, text=danger[0], command=danger[1]).pack(side="left")
            for text, cmd, primary in reversed(buttons):
                ttk.Button(f, text=text, command=cmd, style="Accent.TButton" if primary else "TButton").pack(
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
        def __init__(self, win, card=None, prefill=None):
            s = win.db.get_student(card) if card is not None else None
            Dialog.__init__(self, win, "Edit student" if s else "Register a student")
            self.student = s
            v = s or prefill or {}
            b = self.body
            ttk.Label(b, text="Edit student" if s else "Register a student", style="H2.TLabel").grid(
                row=0, column=0, columnspan=2, sticky="w")
            ttk.Label(b, text="Card number").grid(row=1, column=0, columnspan=2, sticky="w", pady=(10, 2))
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
            self.waiting_lbl = ttk.Label(b, style="Info.TLabel", wraplength=520, text=(
                "Waiting for a new card\u2026 Tap it on the device, then plug the device into this computer. "
                "The number will appear here."))
            self.card_err = ttk.Label(b, style="Err.TLabel")
            self.card_err.grid(row=4, column=0, columnspan=2, sticky="w")
            self.card.trace_add("write", self._check_card)

            ttk.Label(b, text="Full name").grid(row=5, column=0, columnspan=2, sticky="w", pady=(6, 2))
            self.name = tk.StringVar(value=v.get("name", ""))
            self.e_name = ttk.Entry(b, textvariable=self.name, width=56, validate="key", validatecommand=win.vcmd(80))
            self.e_name.grid(row=6, column=0, columnspan=2, sticky="ew")
            ttk.Label(b, text="Student number").grid(row=7, column=0, sticky="w", pady=(8, 2))
            ttk.Label(b, text="Department").grid(row=7, column=1, sticky="w", pady=(8, 2), padx=(10, 0))
            self.no = tk.StringVar(value=v.get("student_no", ""))
            ttk.Entry(b, textvariable=self.no, width=24, validate="key", validatecommand=win.vcmd(40)).grid(
                row=8, column=0, sticky="ew")
            self.dept = tk.StringVar(value=v.get("department", ""))
            ttk.Combobox(b, textvariable=self.dept, values=win.departments, width=28, validate="key",
                         validatecommand=win.vcmd(80)).grid(row=8, column=1, sticky="ew", padx=(10, 0))

            ttk.Label(b, text="Enrolled modules", style="Bold.TLabel").grid(row=9, column=0, columnspan=2, sticky="w",
                                                                           pady=(12, 2))
            self.mods_box = ttk.Frame(b)
            self.mods_box.grid(row=10, column=0, columnspan=2, sticky="ew")
            self.mod_vars = {}
            self.none_lbl = ttk.Label(self.mods_box, text="No modules yet. Add one below.", style="Muted.TLabel")
            have = list(v.get("modules") or [])
            codes = [m["code"] for m in win.modules] + [m for m in have if m not in [x["code"] for x in win.modules]]
            for code in codes:
                self._add_check(code, code in have)
            if not codes:
                self.none_lbl.grid(row=0, column=0, sticky="w")
            ar = ttk.Frame(b)
            ar.grid(row=11, column=0, columnspan=2, sticky="w", pady=(6, 0))
            self.newmod = tk.StringVar()
            self.e_newmod = ttk.Entry(ar, textvariable=self.newmod, width=22, validate="key", validatecommand=win.vcmd(24))
            self.e_newmod.pack(side="left")
            self.e_newmod.bind("<Return>", lambda e: (self.add_mod(), "break")[1])
            ttk.Button(ar, text="Add module", command=self.add_mod).pack(side="left", padx=(6, 0))
            ttk.Label(ar, text="another module code", style="Muted.TLabel").pack(side="left", padx=(6, 0))
            self.err = ttk.Label(b, style="Err.TLabel", wraplength=520)
            self.err.grid(row=12, column=0, columnspan=2, sticky="w", pady=(8, 0))
            self.buttons(13, [("Cancel", self.cancel, False), ("Save", self.ok, True)],
                         danger=("Delete student\u2026", self.delete) if s else None)
            self.show(self.e_name if (s or v.get("card_id")) else self.e_card)

        def _add_check(self, code, on):
            self.none_lbl.grid_forget()
            var = tk.BooleanVar(value=on)
            self.mod_vars[code] = var
            n = len(self.mod_vars) - 1
            ttk.Checkbutton(self.mods_box, text=code, variable=var).grid(row=n // 4, column=n % 4, sticky="w", padx=(0, 14))

        def add_mod(self):
            code = self.newmod.get().strip()
            if code:
                if code not in self.mod_vars:
                    self._add_check(code, True)
                self.newmod.set("")

        def _picked(self, e=None):
            i = self.pick.current()
            if i >= 0:
                self.card.set(card10(self.win.cards[i]["card_id"]))
                self.e_name.focus_set()

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
            mods = [k for k, var in self.mod_vars.items() if var.get()]
            self.err.configure(text="")
            try:
                s = self.win.db.upsert_student(self.card.get(), self.name.get(), self.no.get(), self.dept.get(), mods)
            except D.DbError as e:
                return self.fail(e, self.err)
            self.cancel()
            self.win.changed(s["name"] + " saved. Send the cards to the device so it shows green for this card.", poll=True)

        def delete(self):
            if not self.confirm("Delete this student? Their taps stay in the database but will show as an unregistered card."):
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
        def __init__(self, win):
            Dialog.__init__(self, win, "Import students")
            b = self.body
            ttk.Label(b, text="Import students", style="H2.TLabel").grid(row=0, column=0, columnspan=2, sticky="w")
            ttk.Label(b, wraplength=560, text="Paste rows from Excel, or choose a CSV file. The first line can name the "
                      "columns: card_id, name, student_no, department, modules").grid(row=1, column=0, columnspan=2,
                                                                                   sticky="w", pady=(6, 0))
            ttk.Label(b, style="Muted.TLabel", wraplength=560, text="Modules are separated by semicolons, like "
                      "EN2090;MA1010. Students already in the list are updated.").grid(row=2, column=0, columnspan=2,
                                                                                    sticky="w", pady=(2, 6))
            tf = ttk.Frame(b)
            tf.grid(row=3, column=0, columnspan=2, sticky="nsew")
            self.text = tk.Text(tf, width=72, height=11, wrap="none", undo=True, font="TkFixedFont")
            ys = ttk.Scrollbar(tf, orient="vertical", command=self.text.yview)
            self.text.configure(yscrollcommand=ys.set)
            self.text.pack(side="left", fill="both", expand=True)
            ys.pack(side="right", fill="y")
            fr = ttk.Frame(b)
            fr.grid(row=4, column=0, columnspan=2, sticky="w", pady=(6, 0))
            ttk.Button(fr, text="Choose a file\u2026", command=self.pick).pack(side="left")
            self.file_lbl = ttk.Label(fr, style="Muted.TLabel")
            self.file_lbl.pack(side="left", padx=(8, 0))
            self.result = ttk.Label(b, wraplength=560, justify="left")
            self.result.grid(row=5, column=0, columnspan=2, sticky="w", pady=(8, 0))
            self.buttons(6, [("Close", self.cancel, False), ("Import", self.ok, True)])
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
            try:
                r = self.win.db.import_students(self.text.get("1.0", "end-1c"))
            except D.DbError as e:
                self.result.configure(style="Err.TLabel")
                return self.fail(e, self.result)
            msg = "%s added, %d updated." % (plural(r["added"], "student"), r["updated"])
            errs = ["Line %d: %s" % (x["line"], x["message"]) for x in r["errors"][:8]]
            if len(r["errors"]) > 8:
                errs.append("\u2026and %d more" % (len(r["errors"]) - 8))
            self.result.configure(text="\n".join([msg] + errs), style="Err.TLabel" if errs else "Bold.TLabel")
            self.win.changed(None)

    class ModuleDialog(Dialog):
        def __init__(self, win, code=None):
            m = next((x for x in win.modules if x["code"] == code), None) if code else None
            m = m or {"code": "", "title": "", "department": ""}
            Dialog.__init__(self, win, "Edit module" if code else "Add a module")
            self.old = code or ""
            b = self.body
            ttk.Label(b, text="Edit module" if code else "Add a module", style="H2.TLabel").grid(
                row=0, column=0, columnspan=2, sticky="w")
            ttk.Label(b, text="Code").grid(row=1, column=0, sticky="w", pady=(10, 2))
            ttk.Label(b, text="Department").grid(row=1, column=1, sticky="w", pady=(10, 2), padx=(10, 0))
            self.code = tk.StringVar(value=m["code"])
            self.e_code = ttk.Entry(b, textvariable=self.code, width=20, validate="key", validatecommand=win.vcmd(24))
            self.e_code.grid(row=2, column=0, sticky="ew")
            self.dept = tk.StringVar(value=m["department"])
            ttk.Combobox(b, textvariable=self.dept, values=win.departments, width=30, validate="key",
                         validatecommand=win.vcmd(80)).grid(row=2, column=1, sticky="ew", padx=(10, 0))
            ttk.Label(b, text="Title").grid(row=3, column=0, columnspan=2, sticky="w", pady=(8, 2))
            self.mtitle = tk.StringVar(value=m["title"])
            ttk.Entry(b, textvariable=self.mtitle, width=56, validate="key", validatecommand=win.vcmd(80)).grid(
                row=4, column=0, columnspan=2, sticky="ew")
            self.err = ttk.Label(b, style="Err.TLabel", wraplength=480)
            self.err.grid(row=5, column=0, columnspan=2, sticky="w", pady=(8, 0))
            self.buttons(6, [("Cancel", self.cancel, False), ("Save", self.ok, True)],
                         danger=("Delete\u2026", self.delete) if code else None)
            self.show(self.e_code)

        def ok(self):
            try:
                A.save_module(self.win.db, self.old, self.code.get(), self.mtitle.get(), self.dept.get())
            except (D.DbError, A.ApiError) as e:
                return self.fail(e, self.err)
            self.cancel()
            self.win.changed("Module saved")

        def delete(self):
            if not self.confirm("Delete module %s? Its enrolments go; students and taps stay. If it has lectures, they "
                                "are removed too." % self.old):
                return
            try:
                self.win.db.delete_module(self.old, force=True)
            except D.DbError as e:
                return self.fail(e, self.err)
            self.cancel()
            self.win.changed("Module deleted")

    class EnrolDialog(Dialog):
        def __init__(self, win, code):
            Dialog.__init__(self, win, "Enrolled students")
            self.code = code
            self.people = list(win.students)
            self.on = {s["card_id"]: code in s["modules"] for s in self.people}
            b = self.body
            b.rowconfigure(2, weight=1)
            ttk.Label(b, text="Students enrolled in %s" % code, style="H2.TLabel").grid(row=0, column=0, columnspan=2,
                                                                                     sticky="w")
            r = ttk.Frame(b)
            r.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(8, 6))
            ttk.Label(r, text="Search").pack(side="left")
            self.q = tk.StringVar()
            self.e_q = ttk.Entry(r, textvariable=self.q, width=30)
            self.e_q.pack(side="left", fill="x", expand=True, padx=(6, 6))
            self.q.trace_add("write", lambda *a: self.draw())
            ttk.Button(r, text="Tick shown", command=lambda: self.tick(True)).pack(side="left")
            ttk.Button(r, text="Clear shown", command=lambda: self.tick(False)).pack(side="left", padx=(6, 0))
            self.table = Table(b, [col("on", "\u2713", 44, "center", False), col("name", "Name", 230),
                                   col("no", "Student no", 110), col("dept", "Department", 170)], height=14,
                               selectmode="extended")
            self.table.grid(row=2, column=0, columnspan=2, sticky="nsew")
            self.table.tree.bind("<ButtonRelease-1>", self._click)
            self.table.tree.bind("<space>", lambda e: (self._toggle_selected(), "break")[1])
            self.count = ttk.Label(b, style="Muted.TLabel")
            self.count.grid(row=3, column=0, columnspan=2, sticky="w", pady=(6, 0))
            ttk.Label(b, style="Muted.TLabel", text="Click a row, or press Space, to tick or clear it.").grid(
                row=4, column=0, columnspan=2, sticky="w")
            self.err = ttk.Label(b, style="Err.TLabel", wraplength=520)
            self.err.grid(row=5, column=0, columnspan=2, sticky="w")
            self.buttons(6, [("Cancel", self.cancel, False), ("Save", self.ok, True)])
            self.draw()
            self.show(self.e_q)

        def draw(self):
            qs = self.q.get().strip().lower()
            rows = [(str(s["card_id"]), ("\u2611" if self.on[s["card_id"]] else "\u2610", s["name"], s["student_no"],
                                         s["department"]), ())
                    for s in self.people
                    if not qs or qs in (s["name"] + " " + s["student_no"] + " " + s["department"]).lower()]
            self.table.fill(rows, "No students yet." if not self.people else "Nobody matches that search.")
            self._count()

        def _count(self):
            self.count.configure(text="%d of %d ticked" % (sum(1 for v in self.on.values() if v), len(self.on)))

        def _set(self, iid, value):
            self.on[int(iid)] = value
            self.table.tree.set(iid, "on", "\u2611" if value else "\u2610")

        def _click(self, e):
            if self.table.tree.identify_region(e.x, e.y) == "cell":
                iid = self.table.tree.identify_row(e.y)
                if iid:
                    self._set(iid, not self.on[int(iid)])
                    self._count()

        def _toggle_selected(self):
            for iid in self.table.tree.selection():
                self._set(iid, not self.on[int(iid)])
            self._count()

        def tick(self, value):
            for iid in self.table.tree.get_children():
                self._set(iid, value)
            self._count()

        def ok(self):
            ids = [c for c, v in self.on.items() if v]
            try:
                self.win.db.set_enrollment(self.code, ids, "set")
            except D.DbError as e:
                return self.fail(e, self.err)
            self.cancel()
            self.win.changed("Enrolment saved")

    class LectureDialog(Dialog):
        def __init__(self, win, lecture):
            Dialog.__init__(self, win, "Edit lecture")
            self.lid = lecture["id"]
            b = self.body
            ttk.Label(b, text="Edit lecture", style="H2.TLabel").grid(row=0, column=0, columnspan=2, sticky="w")
            ttk.Label(b, text="Module").grid(row=1, column=0, sticky="w", pady=(10, 2))
            ttk.Label(b, text="Name").grid(row=1, column=1, sticky="w", pady=(10, 2), padx=(10, 0))
            self.module = tk.StringVar(value=lecture["module_code"])
            self.e_module = ttk.Combobox(b, textvariable=self.module, values=[m["code"] for m in win.modules], width=20,
                                         validate="key", validatecommand=win.vcmd(24))
            self.e_module.grid(row=2, column=0, sticky="ew")
            self.ltitle = tk.StringVar(value=lecture["title"])
            ttk.Entry(b, textvariable=self.ltitle, width=36, validate="key", validatecommand=win.vcmd(80)).grid(
                row=2, column=1, sticky="ew", padx=(10, 0))
            ttk.Label(b, text="Starts").grid(row=3, column=0, sticky="w", pady=(8, 2))
            ttk.Label(b, text="Ends (leave empty if still running)").grid(row=3, column=1, sticky="w", pady=(8, 2),
                                                                         padx=(10, 0))
            self.start = tk.StringVar(value=lecture["start_text"])
            ttk.Entry(b, textvariable=self.start, width=20).grid(row=4, column=0, sticky="ew")
            self.end = tk.StringVar(value="" if lecture["running"] else lecture["end_text"])
            ttk.Entry(b, textvariable=self.end, width=20).grid(row=4, column=1, sticky="ew", padx=(10, 0))
            ttk.Label(b, style="Muted.TLabel", wraplength=480, text="Taps between the start and the end count as this "
                      "lecture. Dates look like 2026-10-06 09:00.").grid(row=5, column=0, columnspan=2, sticky="w",
                                                                        pady=(8, 0))
            self.err = ttk.Label(b, style="Err.TLabel", wraplength=480)
            self.err.grid(row=6, column=0, columnspan=2, sticky="w", pady=(6, 0))
            self.buttons(7, [("Cancel", self.cancel, False), ("Save", self.ok, True)])
            self.show(self.e_module)

        def ok(self):
            try:
                A.edit_lecture(self.win.db, self.lid, {"module": self.module.get(), "title": self.ltitle.get(),
                                                       "start": self.start.get(), "end": self.end.get()})
            except (D.DbError, A.ApiError) as e:
                return self.fail(e, self.err)
            self.cancel()
            self.win.changed("Lecture saved")


# --------------------------------------------------------------------------
# The window
# --------------------------------------------------------------------------

class CompanionWindow:
    TABS = ("lecture", "attendance", "students", "modules", "reports")

    def __init__(self, root, app, data_dir=None, demo=False, poll=True):
        self.root, self.app, self.db = root, app, app.db
        self.data_dir, self.demo = data_dir, demo
        self.closing = False
        self.results = queue.Queue()
        self.device_worker = Worker(self.results)      # device scans and writes, one at a time, in order
        self.slow_worker = Worker(self.results)        # PDFs, which can take half a minute
        self.after_ids = {}
        self.busy_n = 0
        self.refreshing = 0
        self.state = None
        self.students, self.departments, self.modules = [], [], []
        self.lectures, self.unassigned, self.cards = [], [], []
        self.sent = None               # the lecture just sent, waiting for the cable to come out
        self.dismissed = set()
        self.waiting = None            # a student dialog waiting for a new card to be read
        self.sending = False
        self.form_touched = False
        self._quiet = False
        self.sel = None                # attendance: 'taps', 'day:YYYY-MM-DD' or 'lec:<id>'
        self.detail = None
        self.taps = []
        self.banner_sig = None
        self.rep_module = ""
        self.rep_cards = []
        self._vcmds = {}
        self.prefs = load_prefs(data_dir)
        self.tmp = tempfile.mkdtemp(prefix="attendance-print-")
        self.print_n = 0

        root.title(APP_TITLE + (" (demo)" if demo else ""))
        w = min(1120, max(800, root.winfo_screenwidth() - 60))
        h = min(760, max(560, root.winfo_screenheight() - 100))
        root.geometry("%dx%d" % (w, h))
        root.minsize(820, 560)
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

    def _job(self, worker, fn, done=None, failed=None, busy=True):
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
        worker.submit(fn, ok, bad)

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
            return "lecture"

    # --------------------------------------------------------------- look
    def _style(self):
        r = self.root
        st = self.style = ttk.Style(r)
        if r.tk.call("tk", "windowingsystem") == "x11" and "clam" in st.theme_names():
            st.theme_use("clam")
        for name in ("TkDefaultFont", "TkTextFont", "TkMenuFont", "TkHeadingFont", "TkCaptionFont", "TkIconFont"):
            try:
                tkfont.nametofont(name).configure(size=10)
            except tk.TclError:
                pass
        base = tkfont.nametofont("TkDefaultFont")
        fam = base.cget("family")
        self.f_h1 = tkfont.Font(family=fam, size=15, weight="bold")
        self.f_h2 = tkfont.Font(family=fam, size=12, weight="bold")
        self.f_big = tkfont.Font(family=fam, size=24, weight="bold")
        self.f_bold = tkfont.Font(family=fam, size=10, weight="bold")
        self.f_small = tkfont.Font(family=fam, size=9)
        st.configure("H1.TLabel", font=self.f_h1)
        st.configure("H2.TLabel", font=self.f_h2)
        st.configure("Big.TLabel", font=self.f_big)
        st.configure("Bold.TLabel", font=self.f_bold)
        st.configure("Muted.TLabel", foreground="#555555")
        st.configure("Err.TLabel", foreground="#b3261e")
        st.configure("Info.TLabel", foreground="#174ea6")
        st.configure("Accent.TButton", font=self.f_bold)
        st.configure("TLabelframe.Label", font=self.f_bold)
        st.configure("Treeview", rowheight=base.metrics("linespace") + 8)
        st.configure("Treeview.Heading", font=self.f_bold)

    def _tags(self, tree):
        tree.tag_configure("warn", foreground="#8a5a00")
        tree.tag_configure("low", foreground="#b3261e")
        tree.tag_configure("bad", foreground="#b3261e")
        tree.tag_configure("ok", foreground="#1e6b35")
        tree.tag_configure("running", foreground="#1e6b35")

    # -------------------------------------------------------------- build
    def _build(self):
        r = self.root
        top = ttk.Frame(r, padding=(12, 8, 12, 4))
        top.pack(side="top", fill="x")
        ttk.Label(top, text=APP_TITLE, style="H1.TLabel").pack(side="left")
        ttk.Button(top, text="Read the device now (F5)", command=lambda: self.poll(force=True)).pack(side="right")
        self.pill = tk.Frame(top, padx=10, pady=4)
        self.pill.pack(side="right", padx=(0, 10))
        self.pill_dot = tk.Label(self.pill, text="\u25cf", font=self.f_h2)
        self.pill_dot.pack(side="left")
        self.pill_title = tk.Label(self.pill, font=self.f_bold)
        self.pill_title.pack(side="left", padx=(4, 0))
        self.pill_detail = tk.Label(self.pill, font=self.f_small)
        self.pill_detail.pack(side="left", padx=(6, 0))

        # bottom first, so the notebook takes what is left
        status = ttk.Frame(r, padding=(12, 2, 12, 4))
        status.pack(side="bottom", fill="x")
        self.msg_var = tk.StringVar()
        self.foot_var = tk.StringVar()
        ttk.Label(status, textvariable=self.foot_var, style="Muted.TLabel").pack(side="right")
        ttk.Label(status, textvariable=self.msg_var, style="Info.TLabel").pack(side="left")
        ttk.Separator(r).pack(side="bottom", fill="x")
        foot = ttk.Frame(r, padding=(12, 6, 12, 4))
        foot.pack(side="bottom", fill="x")
        ttk.Button(foot, text="Import an ATTEND.CSV\u2026", command=self.import_attend).pack(side="left")
        ttk.Button(foot, text="Back up the database\u2026", command=self.backup).pack(side="left", padx=(6, 0))
        self.b_clock = ttk.Button(foot, text="Set the device clock", command=self.set_clock)
        self.b_clock.pack(side="left", padx=(6, 0))
        ttk.Button(foot, text="Quit", command=self.close).pack(side="right")
        ttk.Label(foot, text="Database: %s" % getattr(self.db, "path", ""), style="Muted.TLabel",
                  font=self.f_small).pack(side="right", padx=(10, 10))

        self.banner_box = ttk.Frame(r, padding=(12, 0, 12, 0))
        self.banner_box.pack(side="top", fill="x")

        self.nb = ttk.Notebook(r, padding=(8, 6, 8, 4))
        self.nb.pack(side="top", fill="both", expand=True)
        self.tabs = {}
        for i, (key, text) in enumerate((("lecture", "Lecture"), ("attendance", "Attendance"), ("students", "Students"),
                                         ("modules", "Modules"), ("reports", "Reports"))):
            f = ttk.Frame(self.nb, padding=12)
            self.nb.add(f, text=" %s " % text, underline=1)
            self.tabs[key] = f
        self.nb.enable_traversal()
        self._build_lecture(self.tabs["lecture"])
        self._build_attendance(self.tabs["attendance"])
        self._build_students(self.tabs["students"])
        self._build_modules(self.tabs["modules"])
        self._build_reports(self.tabs["reports"])
        self.nb.bind("<<NotebookTabChanged>>", self._tab_changed)

    def _keys(self):
        r = self.root
        for i in range(len(self.TABS)):
            r.bind("<Control-Key-%d>" % (i + 1), lambda e, i=i: self.nb.select(i))
        r.bind("<F5>", lambda e: self.poll(force=True))
        r.bind("<Control-f>", self.focus_search)
        r.bind("<Control-q>", lambda e: self.close())

    def focus_search(self, e=None):
        t = self.current_tab()
        target = None
        if t == "students":
            target = self.e_st_q
        elif t == "attendance" and self.sel == "taps":
            target = self.e_taps_q
        elif t == "attendance" and self.sel and self.sel.startswith("lec:"):
            target = self.e_lec_q
        if target is not None:
            target.focus_set()
            target.select_range(0, "end")
        return "break"

    # ---- lecture tab
    def _build_lecture(self, tab):
        tab.columnconfigure(0, weight=3)
        tab.columnconfigure(1, weight=2)
        tab.rowconfigure(1, weight=1)
        live = self.live = ttk.LabelFrame(tab, text="Lecture in progress", padding=10)
        live.columnconfigure(0, weight=1)
        self.live_title = ttk.Label(live, style="H2.TLabel")
        self.live_title.grid(row=0, column=0, sticky="w")
        self.live_sub = ttk.Label(live, style="Muted.TLabel")
        self.live_sub.grid(row=1, column=0, sticky="w")
        right = ttk.Frame(live)
        right.grid(row=0, column=1, rowspan=3, sticky="e")
        self.live_num = ttk.Label(right, style="Big.TLabel")
        self.live_num.pack(anchor="e")
        ttk.Label(right, text="present so far", style="Muted.TLabel").pack(anchor="e")
        bb = ttk.Frame(live)
        bb.grid(row=2, column=0, sticky="w", pady=(8, 0))
        ttk.Button(bb, text="See who is here", command=self.open_current).pack(side="left")
        ttk.Button(bb, text="End this lecture", command=self.end_current).pack(side="left", padx=(6, 0))
        ttk.Label(bb, text="Counts update each time the device is plugged in.", style="Muted.TLabel").pack(
            side="left", padx=(10, 0))
        live.grid(row=0, column=0, columnspan=2, sticky="ew", pady=(0, 10))

        form = ttk.LabelFrame(tab, text="Start a lecture", padding=12)
        form.grid(row=1, column=0, sticky="nsew", padx=(0, 10))
        form.columnconfigure(1, weight=1)
        wrapping(ttk.Label(form, style="Muted.TLabel", text="Tell the device which lecture this is. Cards tapped "
                           "afterwards belong to it, and each card is counted once per lecture."), form, margin=40).grid(
            row=0, column=0, columnspan=2, sticky="w")
        ttk.Label(form, text="Module").grid(row=1, column=0, sticky="w", pady=(12, 2))
        ttk.Label(form, text="Lecture").grid(row=1, column=1, sticky="w", pady=(12, 2), padx=(12, 0))
        self.f_module = tk.StringVar()
        self.f_title = tk.StringVar()
        self.f_sync = tk.BooleanVar(value=True)
        self.cb_module = ttk.Combobox(form, textvariable=self.f_module, width=18, validate="key",
                                      validatecommand=self.vcmd(D.MODULE_BYTES))
        self.cb_module.grid(row=2, column=0, sticky="w")
        self.e_title = ttk.Entry(form, textvariable=self.f_title, width=34, validate="key",
                                 validatecommand=self.vcmd(D.LECTURE_BYTES))
        self.e_title.grid(row=2, column=1, sticky="ew", padx=(12, 0))
        self.e_title.bind("<Return>", lambda e: self.start_lecture())
        self.module_hint = ttk.Label(form, style="Muted.TLabel", wraplength=220)
        self.module_hint.grid(row=3, column=0, sticky="nw")
        hint = ttk.Frame(form)
        hint.grid(row=3, column=1, sticky="w", padx=(12, 0))
        ttk.Label(hint, text="Up to 32 letters.", style="Muted.TLabel").pack(side="left")
        self.b_suggest = ttk.Button(hint, command=self.use_suggestion)
        self.b_suggest.pack(side="left", padx=(6, 0))
        self.f_module.trace_add("write", self._form_changed)
        self.f_title.trace_add("write", self._form_changed)
        ttk.Checkbutton(form, text="Set the device clock to this computer\u2019s time", variable=self.f_sync).grid(
            row=4, column=0, columnspan=2, sticky="w", pady=(12, 0))
        self.clock_hint = ttk.Label(form, style="Muted.TLabel")
        self.clock_hint.grid(row=5, column=0, columnspan=2, sticky="w", padx=(24, 0))
        sr = ttk.Frame(form)
        sr.grid(row=6, column=0, columnspan=2, sticky="w", pady=(12, 0))
        self.b_send = ttk.Button(sr, text="Send to device", style="Accent.TButton", command=self.start_lecture)
        self.b_send.pack(side="left", ipadx=10, ipady=2)
        self.send_why = ttk.Label(sr, style="Muted.TLabel")
        self.send_why.pack(side="left", padx=(10, 0))
        rr = ttk.Frame(form)
        rr.grid(row=7, column=0, columnspan=2, sticky="w", pady=(12, 0))
        ttk.Label(rr, text="Device not with you?", style="Muted.TLabel").pack(side="left")
        self.b_record = ttk.Button(rr, text="Record the lecture here only", command=self.record_only)
        self.b_record.pack(side="left", padx=(6, 6))
        ttk.Label(rr, text="(not sent to the device)", style="Muted.TLabel").pack(side="left")

        how = ttk.LabelFrame(tab, text="How a lecture works", padding=12)
        how.grid(row=1, column=1, sticky="nsew")
        steps = [
            "Send to device puts the module and lecture name, and the card numbers of your students, on the drive.",
            "First every tap on the device is saved here, and the device is told to delete its copy. Then the drive "
            "is ejected (if that fails, eject it yourself or press the button on the device once). The device checks "
            "the file: green light and two buzzes = started, red light and three buzzes = refused. The cable can "
            "stay in.",
            "Students tap their cards. The device stores only the card number and the time. Green and a short buzz: a "
            "registered card. Red and a long buzz: a card that is not in your list (it is still recorded, so you can "
            "register it). A second tap in the same lecture gets a double buzz and is ignored.",
            "Plug the device back in. This window reads the taps, matches the cards to your student list, and shows "
            "who came."]
        for i, s in enumerate(steps):
            wrapping(ttk.Label(how, text="%d.  %s" % (i + 1, s), justify="left"), how, margin=40).pack(anchor="w", pady=(0, 6))

    # ---- attendance tab
    def _build_attendance(self, tab):
        pw = self.a_pane = ttk.PanedWindow(tab, orient="horizontal")
        pw.pack(fill="both", expand=True)
        self._sash_set = False

        def first_size(e):
            if not self._sash_set and e.width > 100:
                self._sash_set = True
                pw.sashpos(0, max(300, int(e.width * 0.44)))
        pw.bind("<Configure>", first_size, add="+")
        left = ttk.Frame(pw)
        ttk.Label(left, text="Lectures", style="H2.TLabel").pack(anchor="w", pady=(0, 4))
        self.sessions = Table(left, [col("title", "Lecture", 140), col("module", "Module", 70),
                                     col("when", "When", 118, "w", False), col("present", "Present", 98, "w", False)],
                              height=18, sortable=False)
        self._tags(self.sessions.tree)
        self.sessions.pack(fill="both", expand=True)
        self.sessions.tree.bind("<<TreeviewSelect>>", self._session_picked)
        pw.add(left, weight=2)
        right = ttk.Frame(pw, padding=(12, 0, 0, 0))
        pw.add(right, weight=3)
        self.a_search = tk.StringVar()
        self.a_search.trace_add("write", lambda *a: self.draw_detail_table())

        # nothing to show
        self.d_empty = ttk.Frame(right)
        self.d_empty_title = ttk.Label(self.d_empty, style="H2.TLabel")
        self.d_empty_title.pack(anchor="w", pady=(20, 4))
        self.d_empty_text = wrapping(ttk.Label(self.d_empty, style="Muted.TLabel"), right)
        self.d_empty_text.pack(anchor="w")

        # every tap
        f = self.d_taps = ttk.Frame(right)
        ttk.Label(f, text="Every tap", style="H1.TLabel").pack(anchor="w")
        ttk.Label(f, text="The newest 500, from every lecture and none", style="Muted.TLabel").pack(anchor="w")
        sr = ttk.Frame(f)
        sr.pack(fill="x", pady=8)
        ttk.Label(sr, text="Search name or card").pack(side="left")
        self.e_taps_q = ttk.Entry(sr, textvariable=self.a_search, width=24)
        self.e_taps_q.pack(side="left", padx=(6, 0))
        ttk.Button(sr, text="Save all as CSV\u2026", command=lambda: self.save_now(lambda: A.taps_export(self.db))).pack(
            side="right")
        self.taps_table = Table(f, [col("time", "Time", 150), col("card", "Card", 100), col("name", "Student", 180),
                                    col("dept", "Department", 150)], height=12)
        self._tags(self.taps_table.tree)
        self.taps_table.pack(fill="both", expand=True)

        # a day's taps with no lecture
        f = self.d_day = ttk.Frame(right)
        f.columnconfigure(0, weight=1)
        f.columnconfigure(1, weight=1)
        ttk.Label(f, text="Taps with no lecture set", style="H1.TLabel").grid(row=0, column=0, columnspan=2, sticky="w")
        self.day_info = ttk.Label(f, style="Muted.TLabel")
        self.day_info.grid(row=1, column=0, columnspan=2, sticky="w")
        wrapping(ttk.Label(f, text="These were taken while no lecture was running. Turn them into a lecture and they "
                           "are counted."), right).grid(row=2, column=0, columnspan=2, sticky="w", pady=(10, 10))
        ttk.Label(f, text="Module").grid(row=3, column=0, sticky="w")
        ttk.Label(f, text="Lecture name").grid(row=3, column=1, sticky="w", padx=(10, 0))
        self.u_module = tk.StringVar()
        self.u_title = tk.StringVar()
        self.cb_u_module = ttk.Combobox(f, textvariable=self.u_module, width=20, validate="key",
                                        validatecommand=self.vcmd(D.MODULE_BYTES))
        self.cb_u_module.grid(row=4, column=0, sticky="ew")
        e = ttk.Entry(f, textvariable=self.u_title, width=30, validate="key", validatecommand=self.vcmd(D.LECTURE_BYTES))
        e.grid(row=4, column=1, sticky="ew", padx=(10, 0))
        e.bind("<Return>", lambda ev: self.make_lecture())
        ttk.Button(f, text="Make this a lecture", style="Accent.TButton", command=self.make_lecture).grid(
            row=5, column=0, sticky="w", pady=(12, 0))

        # one lecture
        f = self.d_lec = ttk.Frame(right)
        self.l_title = ttk.Label(f, style="H1.TLabel")
        self.l_title.pack(anchor="w")
        self.l_sub = wrapping(ttk.Label(f, style="Muted.TLabel"), right)
        self.l_sub.pack(anchor="w")
        acts = ttk.Frame(f)
        acts.pack(anchor="w", pady=(8, 0))
        ttk.Button(acts, text="Save PDF\u2026", style="Accent.TButton", command=lambda: self.lec_export("pdf")).pack(
            side="left")
        ttk.Button(acts, text="Save CSV\u2026", command=lambda: self.lec_export("csv")).pack(side="left", padx=(6, 0))
        ttk.Button(acts, text="Print\u2026", command=lambda: self.lec_export("html")).pack(side="left", padx=(6, 0))
        self.l_stats = wrapping(ttk.Label(f, style="Bold.TLabel"), right)
        self.l_stats.pack(anchor="w", pady=(10, 4))
        self.l_bar = ttk.Progressbar(f, maximum=100)
        self.l_bar.pack(fill="x")
        vr = ttk.Frame(f)
        vr.pack(fill="x", pady=8)
        self.view = tk.StringVar(value="present")
        self.view_btns = {}
        for key in ("present", "absent", "other", "unregistered"):
            b = ttk.Radiobutton(vr, variable=self.view, value=key, style="Toolbutton", command=self.draw_detail_table)
            b.pack(side="left", padx=(0, 2))
            self.view_btns[key] = b
        qr = ttk.Frame(f)
        qr.pack(fill="x", pady=(0, 6))
        ttk.Label(qr, text="Search name or card").pack(side="left")
        self.e_lec_q = ttk.Entry(qr, textvariable=self.a_search, width=28)
        self.e_lec_q.pack(side="left", padx=(6, 0))
        br = ttk.Frame(f)
        br.pack(side="bottom", fill="x", pady=(8, 0))
        self.l_table = Table(f, height=5)
        self._tags(self.l_table.tree)
        self.l_table.pack(fill="both", expand=True)
        self.l_table.on_activate(self._lec_row_action)
        self.b_row = ttk.Button(br, command=lambda: self._lec_row_action(self.l_table.selected()))
        ttk.Button(br, text="Edit lecture\u2026", command=self.edit_lecture).pack(side="left")
        self.b_end = ttk.Button(br, text="End lecture", command=lambda: self.end_lecture(self._lec_id()))
        self.b_del = ttk.Button(br, text="Delete lecture\u2026", command=self.delete_lecture)
        self.b_del.pack(side="left", padx=(6, 0))
        self.b_row.pack(side="right")

    # ---- students tab
    def _build_students(self, tab):
        tab.columnconfigure(0, weight=1)
        tab.rowconfigure(1, weight=1)
        nc = self.newcards = ttk.LabelFrame(tab, padding=10)
        wrapping(ttk.Label(nc, style="Muted.TLabel", text="These were tapped but belong to nobody yet. Register each "
                           "one to give it a name, department and modules."), nc).pack(anchor="w")
        row = ttk.Frame(nc)
        row.pack(fill="x", pady=(6, 0))
        self.nc_table = Table(row, [col("card", "Card", 120), col("taps", "Tapped", 90), col("last", "Last tap", 170)],
                              height=2)
        self.nc_table.pack(side="left", fill="x", expand=True)
        self.nc_table.on_activate(lambda iid: self.register(int(iid)))
        ttk.Button(row, text="Register\u2026", style="Accent.TButton",
                   command=lambda: self.nc_table.selected() and self.register(int(self.nc_table.selected()))).pack(
            side="left", padx=(10, 0), anchor="n")
        nc.grid(row=0, column=0, sticky="ew", pady=(0, 6))

        main = ttk.Frame(tab)
        main.grid(row=1, column=0, sticky="nsew")
        main.columnconfigure(0, weight=1)
        main.rowconfigure(2, weight=1)
        head = ttk.Frame(main)
        head.grid(row=0, column=0, sticky="ew")
        hl = ttk.Frame(head)
        hl.pack(side="left")
        ttk.Label(hl, text="Students", style="H1.TLabel").pack(anchor="w")
        cl = ttk.Frame(hl)
        cl.pack(anchor="w")
        self.st_count = ttk.Label(cl, style="Muted.TLabel")
        self.st_count.pack(side="left")
        self.st_devline = ttk.Label(cl, style="Info.TLabel")
        self.st_devline.pack(side="left", padx=(12, 0))
        hb = ttk.Frame(head)
        hb.pack(side="right", anchor="n")
        ttk.Button(hb, text="+ Add student", style="Accent.TButton", command=lambda: StudentDialog(self, None, {})).pack(
            side="left")
        ttk.Button(hb, text="Import\u2026", command=lambda: ImportDialog(self)).pack(side="left", padx=(6, 0))
        self.b_send_cards = ttk.Button(hb, text="Send cards to device", command=self.send_cards)
        self.b_send_cards.pack(side="left", padx=(6, 0))
        ttk.Button(hb, text="Export CSV\u2026", command=lambda: self.save_now(lambda: A.students_export(self.db))).pack(
            side="left", padx=(6, 0))
        filt = ttk.Frame(main)
        filt.grid(row=1, column=0, sticky="ew", pady=8)
        ttk.Label(filt, text="Search").pack(side="left")
        self.st_q = tk.StringVar()
        self.e_st_q = ttk.Entry(filt, textvariable=self.st_q, width=30)
        self.e_st_q.pack(side="left", fill="x", expand=True, padx=(6, 8))
        self.st_q.trace_add("write", lambda *a: self.draw_students())
        self.st_module = ttk.Combobox(filt, state="readonly", width=16)
        self.st_module.pack(side="left")
        self.st_dept = ttk.Combobox(filt, state="readonly", width=24)
        self.st_dept.pack(side="left", padx=(6, 0))
        for cb in (self.st_module, self.st_dept):
            cb.bind("<<ComboboxSelected>>", lambda e: self.draw_students())
        self.st_table = Table(main, [col("name", "Name", 180), col("no", "Student no", 100),
                                     col("dept", "Department", 160), col("modules", "Modules", 150),
                                     col("card", "Card", 100), col("last", "Last tap", 145)], height=10)
        self.st_table.grid(row=2, column=0, sticky="nsew")
        self.st_table.on_activate(lambda iid: StudentDialog(self, int(iid)))
        self.st_table.tree.bind("<Delete>", lambda e: self.delete_student())
        ar = ttk.Frame(main)
        ar.grid(row=3, column=0, sticky="ew", pady=(6, 0))
        ttk.Button(ar, text="Edit\u2026", command=lambda: self.st_table.selected() and StudentDialog(
            self, int(self.st_table.selected()))).pack(side="left")
        ttk.Button(ar, text="Delete\u2026", command=self.delete_student).pack(side="left", padx=(6, 0))
        self.st_help = wrapping(ttk.Label(ar, style="Muted.TLabel", text=(
            "Adding a new student: tap the new card on the device and plug the device into this computer. The card "
            "appears under \u201cNew cards\u201d: press Register, fill in the details and save, then press Send cards "
            "to device, then eject the drive or press the button on the device.")), ar, margin=200)
        self.st_help.pack(side="left", padx=(16, 0))

    # ---- modules tab
    def _build_modules(self, tab):
        tab.columnconfigure(0, weight=1)
        tab.rowconfigure(1, weight=1)
        head = ttk.Frame(tab)
        head.grid(row=0, column=0, sticky="ew", pady=(0, 8))
        hl = ttk.Frame(head)
        hl.pack(side="left")
        ttk.Label(hl, text="Modules", style="H1.TLabel").pack(anchor="w")
        ttk.Label(hl, text="The courses you teach, and who is enrolled in each", style="Muted.TLabel").pack(anchor="w")
        ttk.Button(head, text="+ Add module", style="Accent.TButton", command=lambda: ModuleDialog(self)).pack(
            side="right", anchor="n")
        self.mod_table = Table(tab, [col("code", "Code", 110), col("title", "Title", 260),
                                     col("dept", "Department", 180), col("students", "Students", 90, "center"),
                                     col("lectures", "Lectures", 90, "center")], height=12)
        self.mod_table.grid(row=1, column=0, sticky="nsew")
        self.mod_table.on_activate(lambda iid: ModuleDialog(self, iid))
        ar = ttk.Frame(tab)
        ar.grid(row=2, column=0, sticky="ew", pady=(8, 0))
        ttk.Button(ar, text="Enrolled students\u2026", command=lambda: self.mod_table.selected() and EnrolDialog(
            self, self.mod_table.selected())).pack(side="left")
        ttk.Button(ar, text="Edit\u2026", command=lambda: self.mod_table.selected() and ModuleDialog(
            self, self.mod_table.selected())).pack(side="left", padx=(6, 0))

    # ---- reports tab
    def _build_reports(self, tab):
        tab.columnconfigure(0, weight=1)
        tab.rowconfigure(1, weight=1)
        head = ttk.Frame(tab)
        head.grid(row=0, column=0, sticky="ew")
        hl = ttk.Frame(head)
        hl.pack(side="left")
        ttk.Label(hl, text="Reports", style="H1.TLabel").pack(anchor="w")
        ttk.Label(hl, text="Taps matched to your students, as a page you can print or a file you can keep",
                  style="Muted.TLabel").pack(anchor="w")
        self.rep_mode = tk.StringVar(value="module")
        mb = ttk.Frame(head)
        mb.pack(side="right", anchor="n")
        for value, text in (("module", "By module"), ("student", "By student")):
            ttk.Radiobutton(mb, text=text, value=value, variable=self.rep_mode, style="Toolbutton",
                            command=self.render_reports).pack(side="left", padx=(2, 0))
        body = ttk.Frame(tab)
        body.grid(row=1, column=0, sticky="nsew", pady=(10, 0))
        body.columnconfigure(0, weight=1)
        body.rowconfigure(0, weight=1)

        # by module
        f = self.rep_mod = ttk.Frame(body)
        f.columnconfigure(0, weight=1)
        f.rowconfigure(4, weight=1)
        ctl = ttk.Frame(f)
        ctl.grid(row=0, column=0, sticky="ew")
        ttk.Label(ctl, text="Module").pack(side="left")
        self.cb_rep_module = ttk.Combobox(ctl, state="readonly", width=36)
        self.cb_rep_module.pack(side="left", padx=(6, 14))
        self.cb_rep_module.bind("<<ComboboxSelected>>", self._rep_module_picked)
        self.rep_from = tk.StringVar()
        self.rep_to = tk.StringVar()
        for label, var in (("From", self.rep_from), ("To", self.rep_to)):
            ttk.Label(ctl, text=label).pack(side="left")
            e = ttk.Entry(ctl, textvariable=var, width=12)
            e.pack(side="left", padx=(6, 10))
            e.bind("<Return>", lambda ev: self.load_module_report())
            e.bind("<FocusOut>", lambda ev: self.load_module_report())
        ttk.Button(ctl, text="Show", command=self.load_module_report).pack(side="left")
        ttk.Label(ctl, text="YYYY-MM-DD, optional", style="Muted.TLabel").pack(side="left", padx=(10, 0))
        self.rep_err = ttk.Label(f, style="Err.TLabel")
        self.rep_err.grid(row=1, column=0, sticky="w")
        sr = ttk.Frame(f)
        sr.grid(row=2, column=0, sticky="ew", pady=(6, 6))
        self.rep_stats = ttk.Label(sr, style="Bold.TLabel")
        self.rep_stats.pack(side="left")
        for text, kind, style in (("Print\u2026", "html", "TButton"), ("Save CSV\u2026", "csv", "TButton"),
                                  ("Save PDF\u2026", "pdf", "Accent.TButton")):
            ttk.Button(sr, text=text, style=style, command=lambda k=kind: self.module_export(k)).pack(
                side="right", padx=(6, 0))
        self.rep_none = ttk.Label(f, style="Muted.TLabel")
        self.rep_none.grid(row=3, column=0, sticky="w")
        self.rep_table = Table(f, height=10, hscroll=True)
        self._tags(self.rep_table.tree)
        self.rep_table.grid(row=4, column=0, sticky="nsew")
        self.rep_nomods = ttk.Label(body, style="Muted.TLabel",
                                    text="No modules yet. Add a module and some lectures first.")

        # by student
        f = self.rep_stu = ttk.Frame(body)
        f.columnconfigure(0, weight=1)
        f.rowconfigure(2, weight=1)
        ctl = ttk.Frame(f)
        ctl.grid(row=0, column=0, sticky="ew")
        ttk.Label(ctl, text="Student").pack(side="left")
        self.cb_rep_student = ttk.Combobox(ctl, state="readonly", width=44)
        self.cb_rep_student.pack(side="left", padx=(6, 0))
        self.cb_rep_student.bind("<<ComboboxSelected>>", lambda e: self.load_student_report())
        sr = ttk.Frame(f)
        sr.grid(row=1, column=0, sticky="ew", pady=(8, 6))
        self.rep_s_info = ttk.Label(sr, justify="left")
        self.rep_s_info.pack(side="left")
        self.rep_s_btns = ttk.Frame(sr)
        self.rep_s_btns.pack(side="right", anchor="n")
        for text, kind, style in (("Print\u2026", "html", "TButton"), ("Save CSV\u2026", "csv", "TButton"),
                                  ("Save PDF\u2026", "pdf", "Accent.TButton")):
            ttk.Button(self.rep_s_btns, text=text, style=style, command=lambda k=kind: self.student_export(k)).pack(
                side="right", padx=(6, 0))
        tf = ttk.Frame(f)
        tf.grid(row=2, column=0, sticky="nsew")
        tf.columnconfigure(0, weight=1)
        tf.rowconfigure(0, weight=1)
        self.rep_s_tree = ttk.Treeview(tf, columns=("date", "lecture", "attended", "arrived"), show="tree headings",
                                       height=10)
        self.rep_s_tree.heading("#0", text="Module", anchor="w")
        self.rep_s_tree.column("#0", width=300, stretch=True)
        for key, head, width in (("date", "Date", 110), ("lecture", "Lecture", 240), ("attended", "Attended", 90),
                                 ("arrived", "Arrived", 90)):
            self.rep_s_tree.heading(key, text=head, anchor="w")
            self.rep_s_tree.column(key, width=width, stretch=key == "lecture")
        self._tags(self.rep_s_tree)
        ys = ttk.Scrollbar(tf, orient="vertical", command=self.rep_s_tree.yview)
        self.rep_s_tree.configure(yscrollcommand=ys.set)
        self.rep_s_tree.grid(row=0, column=0, sticky="nsew")
        ys.grid(row=0, column=1, sticky="ns")
        self.rep_s_none = ttk.Label(tf, style="Muted.TLabel")

    # ---------------------------------------------------------- the device
    def poll(self, force=False):
        """Look at the device on the worker thread; again every POLL_MS."""
        if self.closing:
            return
        if force or not self.refreshing:
            self.refreshing += 1
            if force:
                self.toast("Reading the device\u2026")
            self._job(self.device_worker, lambda: self.app.refresh(force), self._got_state, self._poll_failed,
                      busy=force)
        self._later("poll", POLL_MS, self.poll)

    def _poll_failed(self, e):
        self.refreshing = max(0, self.refreshing - 1)
        self.toast("Could not read the device: %s" % err_text(e))

    def _got_state(self, st):
        self.refreshing = max(0, self.refreshing - 1)
        prev, self.state = self.state, st
        changed = prev is None or prev["connected"] != st["connected"] or prev["sync"]["seq"] != st["sync"]["seq"]
        if prev and st["sync"]["seq"] != prev["sync"]["seq"] and st["sync"]["new"] > 0:
            self.toast(plural(st["sync"]["new"], "new tap") + " read from the device")
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

    # --------------------------------------------------------------- data
    def reload(self):
        """Read everything the screens show from the database (the browser version's loadAll)."""
        db = self.db
        try:
            self.students = db.list_students()
            self.departments = db.departments()
            self.modules = db.list_modules()
            self.lectures = db.list_lectures()
            self.unassigned = db.unassigned_days()
            self.cards = db.unregistered_cards()
        except Exception as e:
            self.toast("Could not read the database: %s" % e)
        self.refresh_selected()

    def refresh_selected(self):
        if self.sel and self.sel.startswith("lec:"):
            try:
                self.detail = self.db.lecture_attendance(int(self.sel[4:]))
            except D.DbError:
                self.sel, self.detail = None, None
        elif self.sel == "taps":
            self.taps = self.db.list_taps(limit=500)

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
        connected = bool(st and st.get("connected"))
        self.b_clock.state(["!disabled"] if connected else ["disabled"])
        self.b_send_cards.state(["!disabled"] if connected else ["disabled"])
        self.st_devline.configure(text=cards_line(st))
        c = st["counts"] if st else self.db.counts()
        self.foot_var.set(("Demo data: nothing here is real. " if self.demo else "") + "%s \u00b7 %s \u00b7 %s in the database"
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
            f = tk.Frame(self.banner_box, bg=bg, padx=10, pady=6, highlightthickness=1, highlightbackground=fg)
            f.pack(fill="x", pady=(0, 4))
            if b["action"] == "send-cards":
                ttk.Button(f, text="Send cards to device", style="Accent.TButton", command=self.send_cards).pack(
                    side="right", padx=(10, 0))
            else:
                tk.Button(f, text="\u2715", relief="flat", bg=bg, fg=fg, activebackground=bg, bd=0, cursor="hand2",
                          command=lambda k=b["key"]: self.dismiss(k)).pack(side="right", padx=(10, 0))
            tk.Label(f, text=b["title"], bg=bg, fg=fg, font=self.f_bold).pack(side="left", anchor="n")
            wrapping(tk.Label(f, text=b["text"], bg=bg, fg=fg, justify="left", anchor="w"), f, margin=420).pack(
                side="left", padx=(8, 0), fill="x", expand=True, anchor="w")

    def dismiss(self, key):
        self.dismissed.add(key)
        self.render_banners()

    # ---- lecture
    def render_lecture(self):
        cur = self.db.current_lecture()        # not the last poll's: a lecture may have been ended here since
        if not self.form_touched:
            module = ((self.sent or {}).get("module") or (cur or {}).get("module_code") or self.prefs.get("last_module")
                      or (self.modules[0]["code"] if self.modules else ""))
            self._quiet = True
            self.f_module.set(module)
            self.f_title.set(suggest_title(self.lectures, module, self.sent))
            self._quiet = False
        self.cb_module.configure(values=[m["code"] for m in self.modules])
        if cur:
            info = next((l for l in self.lectures if l["id"] == cur["id"]), None)
            c = info["counts"] if info else {"present_enrolled": 0, "enrolled": 0}
            self.live_title.configure(text=cur["title"])
            self.live_sub.configure(text="%s \u00b7 started %s on %s" % (cur["module_code"], cur["start_text"][11:16], cur["date"]))
            self.live_num.configure(text="%d / %d" % (c["present_enrolled"], c["enrolled"]))
            self.live.grid()
        else:
            self.live.grid_remove()
        self._lecture_controls()

    def _form_changed(self, *a):
        if not self._quiet:
            self.form_touched = True
            self._lecture_controls()

    def _lecture_controls(self):
        st = self.state or {}
        m, t = self.f_module.get().strip(), self.f_title.get().strip()
        if self.sending:
            why = "Sending\u2026"
        elif not st.get("connected"):
            why = "Plug in the device to send the lecture to it."
        elif not m:
            why = "Choose or type a module."
        elif not t:
            why = "Give the lecture a name."
        else:
            why = ""
        self.b_send.state(["disabled"] if why else ["!disabled"])
        self.send_why.configure(text=why)
        self.b_record.state(["!disabled"] if m and t else ["disabled"])
        hint = time.strftime("%Y-%m-%d %H:%M:%S")
        ct = lecture_clock_text(st)
        self.clock_hint.configure(text=hint + (" \u00b7 " + ct if ct else ""))
        self.b_suggest.configure(text="Use \u201c%s\u201d" % suggest_title(self.lectures, self.f_module.get(), self.sent))
        mod = next((x for x in self.modules if x["code"] == m), None)
        if mod:
            self.module_hint.configure(text=mod["title"] or mod["code"])
        else:
            self.module_hint.configure(text="Pick one, or type a new code." if self.modules
                                       else "Type the module code, for example EN2090.")

    def use_suggestion(self):
        self.f_title.set(suggest_title(self.lectures, self.f_module.get(), self.sent))
        self.form_touched = True

    def start_lecture(self):
        if self.b_send.instate(["disabled"]):
            return
        module, title, sync = self.f_module.get(), self.f_title.get(), self.f_sync.get()
        self.sending = True
        self._lecture_controls()

        def done(r):
            self.sending = False
            self.sent = {"module": r["lecture"]["module_code"], "title": r["lecture"]["title"], "done": False,
                         "ejected": r.get("ejected", False), "cleared": r.get("cleared", False)}
            self.dismissed = set()
            self.prefs["last_module"] = r["lecture"]["module_code"]
            save_prefs(self.data_dir, self.prefs)
            self.form_touched = False
            self.changed("Sent and ejected. The lecture starts on the device now." if r.get("ejected") else
                         "Sent. Now eject the drive or press the button on the device.", poll=True)

        def failed(e):
            self.sending = False
            self.show_error(e)
            self.render()
        self._job(self.device_worker, lambda: self.app.start_lecture(module, title, sync), done, failed)

    def record_only(self):
        m, t = self.f_module.get(), self.f_title.get()
        if not (m.strip() and t.strip()):
            return
        try:
            l = self.db.create_lecture(m, t, D.now_ts(), None)
        except D.DbError as e:
            return self.show_error(e)
        self.form_touched = False
        self.changed("Recorded \u201c%s\u201d here only" % l["title"], poll=True)

    def open_current(self):
        cur = self.db.current_lecture()
        if cur:
            self.goto_lecture(cur["id"])

    def goto_lecture(self, lecture_id):
        self.sel = "lec:%d" % lecture_id
        self.a_search.set("")
        self.refresh_selected()
        self.nb.select(self.tabs["attendance"])
        self.render()

    def end_current(self):
        cur = self.db.current_lecture()
        if cur:
            self.end_lecture(cur["id"])

    def end_lecture(self, lecture_id):
        if lecture_id is None:
            return
        try:
            self.db.end_lecture(lecture_id)
        except D.DbError as e:
            return self.show_error(e)
        self.changed("Lecture ended", poll=True)

    # ---- attendance
    def render_attendance(self):
        taps = self.db.tap_count()
        rows = [("taps", ("Every tap", "", "", plural(taps, "tap")), ())]
        for d in self.unassigned:
            rows.append(("day:" + d["date"], ("No lecture set", "", "%s %s\u2013%s" % (d["date"], d["first"][:5], d["last"][:5]),
                                              plural(d["cards"], "card")), ("warn",)))
        for l in self.lectures:
            c = l["counts"]
            present = "%d/%d (%s%%)" % (c["present_enrolled"], c["enrolled"], c["percent"])
            rows.append(("lec:%d" % l["id"], (("\u25b6 " if l["running"] else "") + l["title"], l["module_code"],
                                              "%s %s" % (l["date"], l["start_text"][11:16]), present),
                         ("running",) if l["running"] else ()))
        self.sessions.fill(rows)
        if self.sel and not self.sessions.tree.exists(self.sel):
            self.sel, self.detail = None, None
        if self.sel:
            self.sessions.select(self.sel)
        self.cb_u_module.configure(values=[m["code"] for m in self.modules])
        self.render_detail()

    def _session_picked(self, e=None):
        iid = self.sessions.selected()
        if iid and iid != self.sel:
            self.sel = iid
            self.a_search.set("")
            self.detail = None
            self.refresh_selected()
            self.render_detail()

    def _show_detail(self, frame):
        for f in (self.d_empty, self.d_taps, self.d_day, self.d_lec):
            if f is not frame:
                f.pack_forget()
        frame.pack(fill="both", expand=True)

    def _detail_empty(self, title, text=""):
        self.d_empty_title.configure(text=title)
        self.d_empty_text.configure(text=text)
        self._show_detail(self.d_empty)

    def render_detail(self):
        taps = self.db.tap_count()
        if not self.lectures and not self.unassigned and not taps:
            return self._detail_empty("Nothing recorded yet", "Start a lecture, take attendance with the device, then "
                                      "plug it in. Everything it recorded appears here.")
        if self.sel is None:
            return self._detail_empty("Pick a lecture", "Choose one on the left to see who came.")
        if self.sel == "taps":
            self._show_detail(self.d_taps)
            return self.draw_detail_table()
        if self.sel.startswith("day:"):
            d = next((x for x in self.unassigned if x["date"] == self.sel[4:]), None)
            if not d:
                return self._detail_empty("Pick a lecture", "Choose one on the left to see who came.")
            self.day_info.configure(text="%s \u00b7 %s\u2013%s \u00b7 %s, %s" % (d["date"], d["first"], d["last"],
                                                                             plural(d["cards"], "card"), plural(d["taps"], "tap")))
            if not self.u_module.get():
                self.u_module.set(self.f_module.get())
            return self._show_detail(self.d_day)
        a = self.detail
        if not a:
            return self._detail_empty("Loading\u2026")
        l, c = a["lecture"], a["counts"]
        self.l_title.configure(text=l["title"])
        self.l_sub.configure(text="%s%s \u00b7 %s \u00b7 %s\u2013%s%s" % (
            l["module_code"], (" \u00b7 " + a["module_title"]) if a["module_title"] else "", l["date"],
            l["start_text"][11:16], l["end_text"][11:16], " (running)" if l["running"] else ""))
        bits = ["%d present" % c["present_enrolled"], "%d absent" % c["absent"],
                "%s%% of %d enrolled" % (c["percent"], c["enrolled"])]
        if c["present_other"]:
            bits.append("%d not enrolled here" % c["present_other"])
        if c["unregistered"]:
            bits.append(plural(c["unregistered"], "unregistered card"))
        self.l_stats.configure(text="   \u00b7   ".join(bits))
        self.l_bar.configure(value=min(100, c["percent"]))
        for key, text, n in (("present", "Present", c["present_enrolled"]), ("absent", "Absent", c["absent"]),
                             ("other", "Not enrolled", c["present_other"]), ("unregistered", "Unregistered", c["unregistered"])):
            self.view_btns[key].configure(text="%s (%d)" % (text, n))
        if l["running"]:
            self.b_end.pack(side="left", padx=(6, 0), before=self.b_del)
        else:
            self.b_end.pack_forget()
        self._show_detail(self.d_lec)
        self.draw_detail_table()

    def draw_detail_table(self):
        qs = self.a_search.get()
        if self.sel == "taps":
            rows = [("t%d" % i, (t["time"], card10(t["card_id"]), t["name"] or "Unregistered", t["department"] or ""),
                     () if t["name"] else ("warn",))
                    for i, t in enumerate(self.taps)
                    if matches(qs, [t["name"], t["student_no"], t["department"], t["card_id"], t["time"]])]
            return self.taps_table.fill(rows, "Nothing to show here")
        a = self.detail
        if not a or not (self.sel or "").startswith("lec:"):
            return

        def f(arr):
            return [p for p in arr if matches(qs, [p["name"], p["student_no"], p["department"], p["card_id"]])]
        sc = [col("name", "Name", 180), col("no", "Student no", 100), col("dept", "Department", 140), col("card", "Card", 100)]

        def person(p):
            return [p["name"], p["student_no"], p["department"], card10(p["card_id"])]
        view = self.view.get()
        action = None
        if view == "absent":
            cols, rows = sc + [col("status", "Status", 80)], [(str(p["card_id"]), person(p) + ["Absent"], ("bad",)) for p in f(a["absent"])]
        elif view == "other":
            cols = sc + [col("time", "Arrived", 80)]
            rows = [(str(p["card_id"]), person(p) + [p["time"]], ()) for p in f(a["present_other"])]
            action = "Edit modules\u2026"
        elif view == "unregistered":
            cols = [col("card", "Card", 120), col("time", "Time", 100)]
            rows = [(str(u["card_id"]), [card10(u["card_id"]), u["time"]], ("warn",))
                    for u in a["unregistered"] if matches(qs, [u["card_id"]])]
            action = "Register this card\u2026"
        else:
            cols = sc + [col("time", "Arrived", 80), col("status", "Status", 80)]
            rows = [(str(p["card_id"]), person(p) + [p["time"], "Present"], ("ok",)) for p in f(a["present"])]
        self.l_table.set_columns(cols)
        self.l_table.fill(rows, "Nothing to show here")
        if action:
            self.b_row.configure(text=action)
            self.b_row.pack(side="right")
        else:
            self.b_row.pack_forget()

    def _lec_id(self):
        return self.detail["lecture"]["id"] if self.detail else None

    def _lec_row_action(self, iid):
        if not iid:
            return
        view = self.view.get()
        if view == "other":
            StudentDialog(self, int(iid))
        elif view == "unregistered":
            self.register(int(iid))

    def edit_lecture(self):
        if self.detail:
            LectureDialog(self, self.detail["lecture"])

    def delete_lecture(self):
        lid = self._lec_id()
        if lid is None:
            return
        if not messagebox.askyesno(APP_TITLE, "Delete this lecture? The taps stay in the database; they just stop "
                                   "counting as this lecture.", parent=self.root, icon="warning"):
            return
        try:
            self.db.delete_lecture(lid)
        except D.DbError as e:
            return self.show_error(e)
        self.sel, self.detail = None, None
        self.changed("Lecture deleted")

    def make_lecture(self):
        if not (self.sel or "").startswith("day:"):
            return
        try:
            l = self.db.lecture_from_unassigned(self.sel[4:], self.u_module.get(), self.u_title.get())
        except D.DbError as e:
            return self.show_error(e)
        self.sel = "lec:%d" % l["id"]
        self.u_title.set("")
        self.changed("Lecture \u201c%s\u201d made" % l["title"])

    def lec_export(self, kind):
        lid = self._lec_id()
        if lid is not None:
            self.export(kind, lambda: A.lecture_export(self.db, lid, kind))

    # ---- students
    def render_students(self):
        if self.cards:
            self.newcards.configure(text="New cards seen by the device (%d)" % len(self.cards))
            self.nc_table.fill([(str(c["card_id"]), (card10(c["card_id"]), plural(c["taps"], "time"), c["last"]), ())
                                for c in self.cards])
            self.newcards.grid()
            self.st_help.pack_forget()
        else:
            self.newcards.grid_remove()
            self.st_help.pack(side="left", padx=(16, 0))
        mods = ["All modules"] + [m["code"] for m in self.modules]
        depts = ["All departments"] + self.departments
        self.st_module.configure(values=mods)
        self.st_dept.configure(values=depts)
        if self.st_module.get() not in mods:
            self.st_module.set("All modules")
        if self.st_dept.get() not in depts:
            self.st_dept.set("All departments")
        self.draw_students()

    def draw_students(self):
        qs = self.st_q.get()
        mod = "" if self.st_module.current() <= 0 else self.st_module.get()
        dept = "" if self.st_dept.current() <= 0 else self.st_dept.get()
        rows = [s for s in self.students
                if (not mod or mod in s["modules"]) and (not dept or s["department"] == dept)
                and matches(qs, [s["name"], s["student_no"], s["department"], s["card_id"], card10(s["card_id"])])]
        n = len(self.students)
        self.st_count.configure(text=plural(n, "student") if len(rows) == n else "%d of %s" % (len(rows), plural(n, "student")))
        self.st_table.fill([(str(s["card_id"]), (s["name"], s["student_no"], s["department"],
                                                 " ".join(s["modules"]) or "none", card10(s["card_id"]),
                                                 s["last_tap_text"] or "\u2014"), ()) for s in rows],
                           "No students yet. Add one, import a class list from Excel, or tap a card on the device and "
                           "register it when it appears." if not n else "Nothing to show here")

    def register(self, card_id):
        cur = self.db.current_lecture()
        StudentDialog(self, None, {"card_id": card_id, "modules": [cur["module_code"]] if cur else []})

    def delete_student(self):
        iid = self.st_table.selected()
        if not iid:
            return
        if not messagebox.askyesno(APP_TITLE, "Delete this student? Their taps stay in the database but will show as an "
                                   "unregistered card.", parent=self.root, icon="warning"):
            return
        try:
            self.db.delete_student(int(iid))
        except D.DbError as e:
            return self.show_error(e)
        self.changed("Student deleted")

    def send_cards(self):
        def done(n):
            self.sent = None
            self.toast(plural(n, "card") + " sent. Eject the drive or press the device's button to apply.")
            self.render_banners()
            self.poll()
        self._job(self.device_worker, self.app.send_cards, done)

    # ---- modules
    def render_modules(self):
        self.mod_table.fill([(m["code"], (m["code"], m["title"], m["department"], m["students"], m["lectures"]), ())
                             for m in self.modules],
                            "No modules yet. Add a module, then enrol students in it. Modules are also created when "
                            "you start a lecture or register a student.")

    # ---- reports
    def render_reports(self):
        if self.rep_mode.get() == "student":
            self.rep_mod.grid_remove()
            self.rep_nomods.grid_remove()
            self.rep_stu.grid(row=0, column=0, sticky="nsew")
            card = self.rep_cards[self.cb_rep_student.current()] if self.cb_rep_student.current() >= 0 else None
            self.rep_cards = [s["card_id"] for s in self.students]
            self.cb_rep_student.configure(values=["%s%s" % (s["name"], " (%s)" % s["student_no"] if s["student_no"] else "")
                                                  for s in self.students])
            if card in self.rep_cards:
                self.cb_rep_student.current(self.rep_cards.index(card))
            else:
                self.cb_rep_student.set("Choose a student\u2026")
            return self.load_student_report()
        self.rep_stu.grid_remove()
        if not self.modules:
            self.rep_mod.grid_remove()
            self.rep_nomods.grid(row=0, column=0, sticky="nw")
            return
        self.rep_nomods.grid_remove()
        self.rep_mod.grid(row=0, column=0, sticky="nsew")
        codes = [m["code"] for m in self.modules]
        if self.rep_module not in codes:
            self.rep_module = codes[0]
        self.cb_rep_module.configure(values=["%s%s" % (m["code"], " \u2014 " + m["title"] if m["title"] else "")
                                             for m in self.modules])
        self.cb_rep_module.current(codes.index(self.rep_module))
        self.load_module_report()

    def _rep_module_picked(self, e=None):
        i = self.cb_rep_module.current()
        if 0 <= i < len(self.modules):
            self.rep_module = self.modules[i]["code"]
            self.load_module_report()

    def load_module_report(self):
        if not self.rep_module:
            return
        try:
            r = self.db.module_report(self.rep_module, self.rep_from.get().strip() or None, self.rep_to.get().strip() or None)
        except D.DbError as e:
            self.rep_err.configure(text=str(e))
            self.rep_stats.configure(text="")
            self.rep_none.configure(text="")
            return self.rep_table.fill([])
        self.rep_err.configure(text="")
        s = r["summary"]
        self.rep_stats.configure(text="%s  \u00b7  %d enrolled students  \u00b7  %s%% average attendance  \u00b7  %d below 75%%"
                                 % (plural(s["lectures"], "lecture"), s["students"], s["average"], s["below_75"]))
        cols = [col("name", "Name", 190, "w", False), col("no", "Student no", 100, "w", False)]
        cols += [col("l%d" % i, "%s %s" % (l["date"][5:], l["title"]), 140, "center", False) for i, l in enumerate(r["lectures"])]
        cols += [col("present", "Present", 75, "center", False), col("pct", "%", 70, "center", False)]
        self.rep_table.set_columns(cols)
        if r["lectures"] and r["students"]:
            self.rep_none.configure(text="")
            rows = [(str(st["card_id"]), [st["name"], st["student_no"]] + ["P" if x else "\u2013" for x in st["flags"]] +
                     ["%d/%d" % (st["count"], s["lectures"]), "%s%%" % st["percent"]],
                     ("low",) if st["percent"] < 75 else ()) for st in r["students"]]
        else:
            self.rep_none.configure(text="No lectures in this range. Change the dates, or start a lecture for this module."
                                    if r["students"] else
                                    "Nobody is enrolled in this module. Use Modules \u2192 Enrolled students.")
            rows = []
        self.rep_table.fill(rows)

    def module_export(self, kind):
        code, fr, to = self.rep_module, self.rep_from.get().strip(), self.rep_to.get().strip()
        if code:
            self.export(kind, lambda: A.module_export(self.db, code, kind, fr or None, to or None))

    def load_student_report(self):
        t = self.rep_s_tree
        t.delete(*t.get_children())
        self.rep_s_none.place_forget()
        i = self.cb_rep_student.current()
        if i < 0 or i >= len(self.rep_cards):
            self.rep_s_info.configure(text="")
            self.rep_s_btns.pack_forget()
            return
        try:
            r = self.db.student_report(self.rep_cards[i])
        except D.DbError as e:
            self.rep_s_info.configure(text=str(e))
            return
        st = r["student"]
        self.rep_s_info.configure(text="%s\nCard %s \u00b7 %s \u00b7 %s\n%s recorded%s" % (
            st["name"], card10(st["card_id"]), st["student_no"] or "no student number", st["department"] or "no department",
            plural(r["taps"], "tap"), ", last " + r["last_tap"] if r["last_tap"] else ""))
        self.rep_s_btns.pack(side="right", anchor="n")
        for m in r["modules"]:
            p = t.insert("", "end", text="%s %s \u2014 %d of %d (%s%%)" % (m["code"], m["title"], m["attended"], m["total"],
                                                                         m["percent"]), open=True,
                         tags=("low",) if m["percent"] < 75 and m["total"] else ())
            for l in m["lectures"]:
                t.insert(p, "end", values=(l["date"], l["title"], "Yes" if l["attended"] else "No", l["time"]),
                         tags=("ok",) if l["attended"] else ("bad",))
        if not r["modules"]:
            self.rep_s_none.configure(text="Not enrolled in any module. Enrol this student in a module to see their attendance.")
            self.rep_s_none.place(relx=0.5, rely=0.4, anchor="center")

    def student_export(self, kind):
        i = self.cb_rep_student.current()
        if 0 <= i < len(self.rep_cards):
            card = self.rep_cards[i]
            self.export(kind, lambda: A.student_export(self.db, card, kind))

    # ------------------------------------------------------- files and print
    def export(self, kind, make):
        if kind == "pdf":
            return self.save_pdf(make)
        if kind == "html":
            return self.print_page(make)
        return self.save_now(make)

    def save_now(self, make):
        """Make a file (CSV or a database copy) and ask where to keep it."""
        self.set_busy(True)
        try:
            ex = make()
        except (A.ApiError, D.DbError, OSError) as e:
            return self.show_error(e)
        finally:
            self.set_busy(False)
        self._save_file(ex)

    def save_pdf(self, make):
        if R.find_browser() is None:
            return messagebox.showinfo(APP_TITLE, R.NO_BROWSER, parent=self.root)
        self.toast("Making the PDF\u2026")
        self._job(self.slow_worker, make, self._save_file)

    def _save_file(self, ex):
        data, ctype, name = ex
        ext = os.path.splitext(name)[1].lower()
        types = {".csv": [("CSV files", "*.csv")], ".pdf": [("PDF files", "*.pdf")],
                 ".db": [("Database files", "*.db")]}.get(ext, []) + [("All files", "*")]
        folder = self.prefs.get("save_dir")
        path = filedialog.asksaveasfilename(
            parent=self.root, title="Save " + name, initialfile=name, defaultextension=ext, filetypes=types,
            initialdir=folder if folder and os.path.isdir(folder) else os.path.expanduser("~"))
        if not path:
            self.toast("")
            return
        try:
            with open(path, "wb") as f:
                f.write(data)
        except OSError as e:
            return self.show_error(e)
        self.prefs["save_dir"] = os.path.dirname(path)
        save_prefs(self.data_dir, self.prefs)
        self.toast("Saved %s" % path)

    def print_page(self, make):
        """Open the printable page in the browser, which shows its Print window by itself."""
        try:
            data, ctype, name = make()
        except (A.ApiError, D.DbError) as e:
            return self.show_error(e)
        self.print_n += 1
        page = os.path.join(self.tmp, "report-%d.html" % self.print_n)
        launch = os.path.join(self.tmp, "print-%d.html" % self.print_n)
        try:
            with open(page, "wb") as f:
                f.write(data)
            # A local page cannot be opened with ?print=1 on every system, but it can send the browser there.
            with open(launch, "w", encoding="utf-8") as f:
                f.write('<!doctype html><meta charset="utf-8"><title>Printing</title>'
                        '<meta http-equiv="refresh" content="0;url=%s?print=1">'
                        '<a href="%s?print=1">Open the report</a>' % (os.path.basename(page), os.path.basename(page)))
        except OSError as e:
            return self.show_error(e)
        if webbrowser.open(pathlib.Path(launch).resolve().as_uri()):
            self.toast("The report opened in your browser. In its Print window choose a printer, or Save as PDF.")
        else:
            messagebox.showinfo(APP_TITLE, "No browser could be opened. The report is saved at\n%s" % page, parent=self.root)

    def import_attend(self):
        path = filedialog.askopenfilename(parent=self.root, title="Import an ATTEND.CSV",
                                          filetypes=[("ATTEND.CSV", "*.csv *.CSV"), ("Text", "*.txt *.TXT"), ("All files", "*")])
        if not path:
            return
        try:
            r = A.import_taps(self.db, A.read_text(path))
        except (A.ApiError, D.DbError, OSError) as e:
            return self.show_error(e)
        self.changed("%s imported (%d in the file)" % (plural(r["new"], "new tap"), r["read"]))

    def backup(self):
        self.save_now(lambda: A.backup_export(self.db))

    def set_clock(self):
        def done(_):
            self.sent = None
            self.toast("Clock sent. Eject the drive or press the device's button to apply it.")
            self.render_banners()
        self._job(self.device_worker, self.app.set_clock, done)

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
        self.slow_worker.stop(0.5)
        save_prefs(self.data_dir, self.prefs)
        shutil.rmtree(self.tmp, ignore_errors=True)
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

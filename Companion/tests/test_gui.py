"""Tests for attendance_gui.py: its plain helpers, and a smoke test of the real window (skipped with no display).
Run from the Companion folder:  python -m unittest discover -s tests -v"""
import os
import queue
import shutil
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import attendance_app as A  # noqa: E402
import attendance_db as D  # noqa: E402
import attendance_gui as G  # noqa: E402


def lec(title, module="EN2090"):
    return {"title": title, "module_code": module}


class TestHelpers(unittest.TestCase):
    def test_suggest_title(self):
        lectures = [lec("Circuits Lecture 4"), lec("Calculus 9", "MA1010"), lec("Circuits Lecture 3")]
        self.assertEqual(G.suggest_title(lectures, "EN2090"), "Circuits Lecture 5")
        self.assertEqual(G.suggest_title(lectures, "MA1010"), "Calculus 10")
        self.assertEqual(G.suggest_title(lectures, "CS1010"), "Lecture 1")
        self.assertEqual(G.suggest_title([lec("Intro")], "EN2090"), "Intro 2")
        self.assertEqual(G.suggest_title(lectures, "EN2090", {"module": "EN2090", "title": "Lab 7"}), "Lab 8")

    def state(self, **kw):
        st = {"connected": True, "drift_seconds": 0, "path": "/media/x/ATTENDANCE",
              "device": {"records": 3, "error": "", "pending": False, "has_lecture": False},
              "cards": {"device": 30, "database": 30, "in_sync": True, "too_many": False}}
        st.update(kw)
        return st

    def test_banners(self):
        self.assertEqual(G.banner_list(None, None, "lecture"), [])
        self.assertEqual(G.banner_list(self.state(), None, "students"), [])
        st = self.state(cards={"device": 30, "database": 31, "in_sync": False, "too_many": False})
        b = G.banner_list(st, None, "students")
        self.assertEqual([(x["key"], x["action"]) for x in b], [("cards", "send-cards")])
        self.assertIn("It knows 30 and the list has 31", b[0]["text"])
        self.assertEqual(G.banner_list(st, None, "students", {"cards"})[0]["key"], "cards", "cannot be dismissed")
        st = self.state(drift_seconds=-600)
        self.assertEqual(G.banner_list(st, None, "lecture"), [], "the Lecture tab shows the clock itself")
        self.assertIn("10 minutes behind", G.banner_list(st, None, "students")[0]["title"])
        st = self.state(device={"records": 0, "error": "ERROR, bad line 3", "pending": True, "has_lecture": False},
                        cards={"device": 30, "database": 31, "in_sync": False, "too_many": False})
        keys = [x["key"] for x in G.banner_list(st, None, "lecture")]
        self.assertEqual(keys, ["err:ERROR, bad line 3", "pending"], "no cards banner while changes are pending")
        self.assertEqual(G.banner_list(st, None, "lecture")[0]["text"], "bad line 3")
        self.assertEqual(G.banner_list(st, None, "lecture", {"pending"})[-1]["key"], "err:ERROR, bad line 3")
        st = self.state(cards={"device": 30, "database": 1001, "in_sync": False, "too_many": True})
        self.assertEqual([x["key"] for x in G.banner_list(st, None, "x")], ["cardsmany"])
        sent = {"module": "EN2090", "title": "L5", "done": False}
        self.assertEqual(G.banner_list(None, sent, "x")[0]["title"], "Sent to the device.")
        sent["done"] = True
        self.assertIn("started on the device", G.banner_list(None, sent, "x")[0]["title"])

    def test_status_texts(self):
        self.assertEqual(G.device_summary(None)[0], "off")
        self.assertEqual(G.device_summary({"connected": False})[:2], ("wait", "Waiting for the device"))
        kind, title, detail = G.device_summary(self.state(drift_seconds=200), demo=True)
        self.assertEqual((kind, title), ("ok", "Demo device"))
        self.assertIn("3 taps on the device", detail)
        self.assertIn("clock ahead 3 min", detail)
        self.assertEqual(G.cards_line(self.state()), "The device knows all 30 cards.")
        self.assertEqual(G.cards_line({"connected": False}), "")
        self.assertEqual(G.cards_line(self.state(cards={"device": None, "database": 3})), "")
        self.assertEqual(G.lecture_clock_text(self.state(drift_seconds=60)), "The device clock is right.")
        self.assertEqual(G.lecture_clock_text(self.state(drift_seconds=-150)), "The device clock is 3 minutes behind.")
        self.assertEqual(G.lecture_clock_text(self.state(drift_seconds=None)), "")

    def test_small_things(self):
        self.assertEqual(G.plural(1, "tap"), "1 tap")
        self.assertEqual(G.plural(2, "tap"), "2 taps")
        self.assertEqual(G.card10(42), "0000000042")
        self.assertTrue(G.matches("", ["x"]))
        self.assertTrue(G.matches("per", ["Amal Perera", None]))
        self.assertTrue(G.matches("1000", [1000]))
        self.assertFalse(G.matches("zz", ["Amal", None, 5]))
        self.assertEqual(G.err_text(A.ApiError(409, "Not here")), "Not here")
        self.assertEqual(G.err_text(D.DbError("Refused")), "Refused")

    def test_prefs(self):
        d = tempfile.mkdtemp(prefix="att-prefs-")
        try:
            self.assertEqual(G.load_prefs(d), {})
            G.save_prefs(d, {"last_module": "EN2090"})
            self.assertEqual(G.load_prefs(d), {"last_module": "EN2090"})
            with open(os.path.join(d, G.PREFS_NAME), "w") as f:
                f.write("not json")
            self.assertEqual(G.load_prefs(d), {})
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_one_window_per_data_folder(self):
        d = tempfile.mkdtemp(prefix="att-lock-")
        try:
            first, second = G.InstanceLock(d), G.InstanceLock(d)
            self.assertTrue(first.acquire())
            self.assertFalse(second.acquire(), "a second copy on the same folder is refused")
            other = tempfile.mkdtemp(prefix="att-lock2-")
            try:
                third = G.InstanceLock(other)
                self.assertTrue(third.acquire(), "another folder is fine")
                third.release()
            finally:
                shutil.rmtree(other, ignore_errors=True)
            first.release()
            self.assertTrue(second.acquire(), "free again once the first has gone")
            second.release()
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_worker_hands_results_back(self):
        results = queue.Queue()
        w = G.Worker(results)
        try:
            w.submit(lambda: 6 * 7, "done", "failed")
            w.submit(lambda: 1 / 0, "done", "failed")
            self.assertEqual(results.get(timeout=5), ("done", 42))
            cb, err = results.get(timeout=5)
            self.assertEqual(cb, "failed")
            self.assertIsInstance(err, ZeroDivisionError)
        finally:
            w.stop()
        self.assertFalse(w.thread.is_alive())


def _display_reason():
    if G.tk is None:
        return "this Python has no Tk"
    if sys.platform.startswith("linux") and not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        return "no display"
    try:
        r = G.tk.Tk()
        r.destroy()
    except G.tk.TclError as e:
        return "no display (%s)" % e
    return None


_NO_DISPLAY = _display_reason()


@unittest.skipIf(_NO_DISPLAY, _NO_DISPLAY or "")
class TestWindowSmoke(unittest.TestCase):
    """The real window on demo data: every tab, a refresh, the dialogs, a device write, and closing."""

    def setUp(self):
        self.data = tempfile.mkdtemp(prefix="att-gui-data-")
        self.dev = tempfile.mkdtemp(prefix="att-gui-dev-")
        A.make_demo(self.data, self.dev)
        self.db = D.Database(os.path.join(self.data, "attendance.db"))
        self.root = G.tk.Tk()
        self.errors = []
        self.root.report_callback_exception = lambda *exc: self.errors.append(exc)
        self.win = G.CompanionWindow(self.root, A.App(self.db, self.dev, self.data), self.data, demo=True)

    def tearDown(self):
        self.win.close()
        self.db.close()
        shutil.rmtree(self.data, ignore_errors=True)
        shutil.rmtree(self.dev, ignore_errors=True)

    def pump(self, until=None, timeout=20.0):
        end = time.time() + timeout
        while time.time() < end:
            self.root.update()
            if until is not None and until():
                return True
            time.sleep(0.01)
        return until is None

    def check(self):
        if self.errors:
            import traceback
            self.fail("".join(traceback.format_exception(*self.errors[0])))

    def test_tabs_refresh_dialogs_and_close(self):
        win = self.win
        self.assertTrue(self.pump(lambda: win.state is not None), "the first look at the device came back")
        self.assertTrue(win.state["connected"])
        self.assertGreater(self.db.tap_count(), 0, "the taps were read in")
        for i, name in enumerate(win.TABS):
            win.nb.select(i)
            self.pump(timeout=0.2)
            self.assertEqual(win.current_tab(), name)
        seq = win.state["sync"]["seq"]
        win.poll(force=True)
        self.assertTrue(self.pump(lambda: win.state["sync"]["seq"] > seq), "a forced refresh reads the device again")

        # attendance: a lecture, each of its lists, every tap, and a day with no lecture
        win.goto_lecture(self.db.current_lecture()["id"])
        self.pump(timeout=0.2)
        for view in ("present", "absent", "other", "unregistered"):
            win.view.set(view)
            win.draw_detail_table()
        self.assertEqual(len(win.l_table.rows), 2, "the demo's two new cards")
        win.sessions.select("taps")
        self.pump(timeout=0.2)
        self.assertEqual(win.sel, "taps")
        win.a_search.set("Amal")
        self.assertTrue(all("Amal" in r[1][2] for r in win.taps_table.rows))
        win.sessions.select("day:" + win.unassigned[0]["date"])
        self.pump(timeout=0.2)

        # reports, both ways
        win.nb.select(4)
        self.pump(timeout=0.2)
        self.assertTrue(win.rep_table.rows)
        win.rep_mode.set("student")
        win.render_reports()
        win.cb_rep_student.current(0)
        win.load_student_report()
        self.assertTrue(win.rep_s_tree.get_children())

        # every dialog opens and closes
        win.nb.select(2)
        self.pump(timeout=0.2)
        for make in (lambda: G.StudentDialog(win, None, {}), lambda: G.StudentDialog(win, 1000),
                     lambda: G.ImportDialog(win), lambda: G.ModuleDialog(win), lambda: G.ModuleDialog(win, "EN2090"),
                     lambda: G.EnrolDialog(win, "EN2090"), lambda: G.LectureDialog(win, self.db.list_lectures()[0])):
            d = make()
            self.pump(timeout=0.1)
            d.cancel()

        # a write to the device goes through the worker and comes back
        before = win.msg_var.get()
        win.send_cards()
        self.assertTrue(self.pump(lambda: "cards sent" in win.msg_var.get() and win.msg_var.get() != before))
        with open(os.path.join(self.dev, A.SETTINGS_NAME), encoding="utf-8") as f:
            self.assertIn("#CARDS,30", f.read())
        self.check()


if __name__ == "__main__":
    unittest.main()

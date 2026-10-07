"""Tests for the actions and exports in attendance_app.py that the web routes and the desktop window share.
Run from the Companion folder:  python -m unittest discover -s tests -v"""
import os
import re
import shutil
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import attendance_app as A  # noqa: E402
import attendance_db as D  # noqa: E402
import report_pages as R  # noqa: E402


def settings(dev):
    with open(os.path.join(dev, A.SETTINGS_NAME), "r", encoding="utf-8", newline="") as f:
        return f.read()


class DemoCase(unittest.TestCase):
    """The demo database and device, read once, with no server."""

    def setUp(self):
        self.data = tempfile.mkdtemp(prefix="att-act-data-")
        self.dev = tempfile.mkdtemp(prefix="att-act-dev-")
        A.make_demo(self.data, self.dev)
        self.db = D.Database(os.path.join(self.data, "attendance.db"))
        self.app = A.App(self.db, self.dev, self.data)
        self.state = self.app.refresh()

    def tearDown(self):
        self.db.close()
        shutil.rmtree(self.data, ignore_errors=True)
        shutil.rmtree(self.dev, ignore_errors=True)

    def lecture(self, title):
        return [l for l in self.db.list_lectures() if l["title"] == title][0]


class TestDeviceActions(DemoCase):
    def test_start_lecture(self):
        r = self.app.start_lecture("MA1010", "Calculus Lecture 3")
        self.assertTrue(r["clock_set"])
        self.assertEqual(r["lecture"]["title"], "Calculus Lecture 3")
        text = settings(self.dev)
        self.assertIn("\r\n#NEWSESSION,1\r\n", text)
        self.assertTrue(r["cleared"], "every tap was imported, so the device may delete them")
        self.assertIn("\r\n#CLEARLOG,1\r\n", text)
        self.assertFalse(r["ejected"], "the demo folder is never ejected")
        self.assertIn("\r\n#TIME,", text)
        self.assertEqual(self.db.current_lecture()["module_code"], "MA1010")

    def test_start_lecture_imports_before_clearing(self):
        rows, _ = D.parse_attend(A.read_text(os.path.join(self.dev, A.ATTEND_NAME)))
        with mock.patch.object(self.db, "add_taps", wraps=self.db.add_taps) as add:
            self.app.start_lecture("MA1010", "Calculus Lecture 3")
        self.assertTrue(add.called, "the taps are read into the database before the device is told to delete them")
        stored = set((t["ts"], t["card_id"]) for t in self.db.list_taps())
        self.assertTrue(all((ts, c) in stored for ts, c in rows), "every row on the device is in the database")

    def test_start_lecture_keeps_the_log_when_the_read_is_short(self):
        # STATUS.TXT promising more rows than ATTEND.CSV holds: a partial read must not clear the device.
        path = os.path.join(self.dev, A.STATUS_NAME)
        text = A.read_text(path)
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(re.sub(r"Attendance   : \d+", "Attendance   : 99999", text))
        r = self.app.start_lecture("MA1010", "Calculus Lecture 3")
        self.assertFalse(r["cleared"])
        self.assertNotIn("#CLEARLOG", settings(self.dev))

    def test_start_lecture_can_keep_the_log(self):
        r = self.app.start_lecture("MA1010", "Calculus Lecture 3", clear_device=False)
        self.assertFalse(r["cleared"])
        self.assertNotIn("#CLEARLOG", settings(self.dev))

    def test_start_lecture_without_the_clock_uses_the_devices_time(self):
        self.app.pc_minus_device = 600
        before = D.now_ts()
        r = self.app.start_lecture("MA1010", "Offset", sync_clock=False)
        self.assertFalse(r["clock_set"])
        self.assertNotIn("\r\n#TIME,", settings(self.dev))
        self.assertAlmostEqual(r["lecture"]["start_ts"], before - 600, delta=5)

    def test_start_lecture_refusals(self):
        for module, title, msg in (("", "L", "Choose or enter a module"), ("M", " ", "Enter a lecture name")):
            with self.assertRaises(A.ApiError) as cm:
                self.app.start_lecture(module, title)
            self.assertEqual((cm.exception.code, cm.exception.message), (400, msg))
        n = self.db.counts()["lectures"]
        os.rename(os.path.join(self.dev, A.STATUS_NAME), os.path.join(self.dev, "gone"))
        self.app.last_path = None
        with self.assertRaises(A.ApiError) as cm:
            self.app.start_lecture("MA1010", "Nowhere")
        self.assertEqual(cm.exception.code, 409)
        self.assertEqual(self.db.counts()["lectures"], n, "nothing recorded when the device could not be written")

    def test_send_cards_and_clock(self):
        self.assertEqual(self.app.send_cards(), 30)
        text = settings(self.dev)
        self.assertIn("\r\n#CARDS,30\r\n", text)
        self.assertNotIn("#TIME", text)
        self.app.set_clock()
        text = settings(self.dev)
        self.assertIn("\r\n#TIME,", text)
        self.assertIn("\r\n#CARDS,30\r\n", text)
        self.assertNotIn("\r\n#NEWSESSION", text)

    def test_device_number(self):
        self.app.set_device_id("77")
        self.assertIn("\r\n#DEVICE,0000000077\r\n", settings(self.dev))
        for bad in (-1, 4294967295, "abc", None):
            with self.assertRaises(A.ApiError):
                self.app.set_device_id(bad)


class TestDataActions(DemoCase):
    def test_import_taps(self):
        text = "DATE,TIME,CARD_ID\r\n2027-01-01,09:00:00,0000001000\r\n2027-01-01,09:00:05,0000001007\r\n"
        self.assertEqual(A.import_taps(self.db, text), {"read": 2, "new": 2, "skipped": 0})
        self.assertEqual(A.import_taps(self.db, text)["new"], 0)
        with self.assertRaises(A.ApiError) as cm:
            A.import_taps(self.db, "not an attendance file")
        self.assertIn("No taps", cm.exception.message)
        with self.assertRaises(A.ApiError):
            A.import_taps(self.db, 5)

    def test_edit_lecture(self):
        l = self.db.create_lecture("EN2090", "Make-up", D.parse_ts("2026-01-05 10:00"), D.parse_ts("2026-01-05 11:00"))
        l = A.edit_lecture(self.db, l["id"], {"title": "Moved", "end": "2026-01-05 11:30"})
        self.assertEqual((l["title"], l["module_code"], l["end_text"]), ("Moved", "EN2090", "2026-01-05 11:30:00"))
        self.assertTrue(A.edit_lecture(self.db, l["id"], {"end": ""})["running"])
        with self.assertRaises(D.DbError):
            A.edit_lecture(self.db, l["id"], {"start": "not a date"})
        with self.assertRaises(D.DbError):
            A.edit_lecture(self.db, 99999, {"title": "x"})

    def test_save_module(self):
        m = A.save_module(self.db, "", "PH1010", "Physics", "Science")
        self.assertEqual((m["code"], m["title"]), ("PH1010", "Physics"))
        m = A.save_module(self.db, "PH1010", "PH1011", "Physics II", "Science")
        self.assertEqual(m["code"], "PH1011")
        self.assertIsNone(self.db.get_module("PH1010"))
        m = A.save_module(self.db, "EN2090", " EN2090 ", "Renamed title", "")
        self.assertEqual(m["title"], "Renamed title")
        self.assertEqual(self.db.get_module("EN2090")["title"], "Renamed title", "spaces around the code are not a rename")
        with self.assertRaises(D.DbError):
            A.save_module(self.db, "PH1011", "MA1010", "", "")
        with self.assertRaises(D.DbError):
            A.save_module(self.db, "", "  ", "", "")

    def test_opt_ts(self):
        self.assertIsNone(A.opt_ts(""))
        self.assertIsNone(A.opt_ts(None))
        self.assertIsNone(A.opt_ts("   "))
        self.assertEqual(A.opt_ts(" 2026-01-05 10:00 "), D.parse_ts("2026-01-05 10:00"))


class TestExports(DemoCase):
    def test_lecture(self):
        l = self.lecture("Circuits Lecture 1")
        data, ctype, name = A.lecture_export(self.db, l["id"], "csv")
        self.assertEqual(ctype, A.CSV_TYPE)
        self.assertTrue(name.endswith("_EN2090_Circuits_Lecture_1.csv"), name)
        self.assertTrue(data.startswith(b"\xef\xbb\xbfModule,EN2090"))
        data, ctype, name = A.lecture_export(self.db, l["id"], "html")
        self.assertEqual(ctype, A.HTML_TYPE)
        self.assertIn(b"<h1>Circuits Lecture 1</h1>", data)
        with self.assertRaises(D.DbError):
            A.lecture_export(self.db, 99999, "csv")

    def test_module_and_student(self):
        data, _, name = A.module_export(self.db, "EN2090", "csv")
        self.assertEqual(name, "EN2090_attendance.csv")
        self.assertTrue(data.decode("utf-8-sig").startswith("CARD_ID,NAME,STUDENT_NO,DEPARTMENT"))
        data, _, _ = A.module_export(self.db, "EN2090", "csv", "2099-01-01", "")
        self.assertEqual(data.decode("utf-8-sig").count("\r\n"), 1 + 20, "no lecture columns in that range")
        with self.assertRaises(D.DbError):
            A.module_export(self.db, "EN2090", "csv", "1st of May")
        data, _, name = A.student_export(self.db, 1000, "csv")
        self.assertEqual(name, "Amal_Perera_attendance.csv")
        self.assertIn(b"Amal Perera", A.student_export(self.db, 1000, "html")[0])

    def test_lists_and_backup(self):
        data, _, name = A.students_export(self.db)
        self.assertEqual(name, "students.csv")
        self.assertTrue(data.decode("utf-8-sig").startswith("card_id,name"))
        data, _, name = A.taps_export(self.db)
        self.assertEqual(name, "taps.csv")
        self.assertEqual(data.decode("utf-8-sig").count("\r\n"), 1 + self.db.tap_count())
        data, _, name = A.backup_export(self.db)
        self.assertTrue(data.startswith(b"SQLite format 3"))
        self.assertRegex(name, r"^attendance-\d{8}-\d{4}\.db$")

    def test_pdf_without_a_browser(self):
        l = self.lecture("Circuits Lecture 1")
        with mock.patch.object(R, "find_browser", return_value=None):
            with self.assertRaises(A.ApiError) as cm:
                A.lecture_export(self.db, l["id"], "pdf")
        self.assertEqual((cm.exception.code, cm.exception.message), (501, R.NO_BROWSER))


if __name__ == "__main__":
    unittest.main()

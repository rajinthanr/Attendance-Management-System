"""Tests for attendance_app.py.   Run from the Companion folder:  python -m unittest discover -s tests -v"""
import http.client
import json
import os
import shutil
import sqlite3
import sys
import tempfile
import threading
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)

import attendance_app as app  # noqa: E402
import attendance_db as D  # noqa: E402
import report_pages as R  # noqa: E402


def rb(path):
    with open(path, "rb") as f:
        return f.read()


def rt(path):
    with open(path, "r", encoding="utf-8", newline="") as f:
        return f.read()


class ServerCase(unittest.TestCase):
    """A real server on a free port, a real database, and a demo 'device' folder."""

    def setUp(self):
        self.data = tempfile.mkdtemp(prefix="att-data-")
        self.dev = tempfile.mkdtemp(prefix="att-dev-")
        app.make_demo(self.data, self.dev)
        self.db = D.Database(os.path.join(self.data, "attendance.db"))
        self.app = app.App(self.db, self.dev, self.data)
        app.Handler.app = self.app
        self.server = app.ThreadingHTTPServer(("127.0.0.1", 0), app.Handler)
        self.app.server = self.server
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.db.close()
        shutil.rmtree(self.data, ignore_errors=True)
        shutil.rmtree(self.dev, ignore_errors=True)

    # ---- helpers
    def raw(self, method, path, body=None, headers=None, host=None):
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=30)
        h = dict(headers or {})
        h["Host"] = host or ("127.0.0.1:%d" % self.port)
        if method != "GET":
            h.setdefault("X-Attendance", "1")
        if body is not None and not isinstance(body, bytes):
            body = json.dumps(body).encode("utf-8")
            h.setdefault("Content-Type", "application/json")
        c.request(method, path, body=body, headers=h)
        r = c.getresponse()
        data = r.read()
        hdrs = dict(r.getheaders())
        c.close()
        return r.status, data, hdrs

    def api(self, method, path, body=None, expect=200, **kw):
        code, data, _ = self.raw(method, path, body, **kw)
        self.assertEqual(code, expect, "%s %s -> %s %s" % (method, path, code, data[:300]))
        try:
            return json.loads(data)
        except ValueError:
            return data

    def get(self, path, **kw):
        return self.api("GET", path, **kw)

    def sync(self):
        return self.get("/api/state")

    def settings_text(self):
        return rt(os.path.join(self.dev, app.SETTINGS_NAME))


class TestDetection(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="att-det-")

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def test_empty_folder_is_not_a_device(self):
        self.assertFalse(app.is_device(self.dir))
        self.assertIsNone(app.find_device(self.dir))

    def make(self, d):
        data = tempfile.mkdtemp(prefix="att-det-data-")
        try:
            app.make_demo(data, d)
        finally:
            shutil.rmtree(data, ignore_errors=True)

    def test_demo_device_is_recognised(self):
        self.make(self.dir)
        self.assertTrue(app.is_device(self.dir))
        self.assertEqual(app.find_device(self.dir), self.dir)

    def test_missing_any_file_is_not_a_device(self):
        for name in (app.STATUS_NAME, app.ATTEND_NAME, app.SETTINGS_NAME):
            d = tempfile.mkdtemp(prefix="att-det2-")
            try:
                self.make(d)
                os.remove(os.path.join(d, name))
                self.assertFalse(app.is_device(d), name)
            finally:
                shutil.rmtree(d, ignore_errors=True)

    def test_other_drives_with_the_same_file_names_are_not_taken(self):
        for n in (app.STATUS_NAME, app.ATTEND_NAME, app.SETTINGS_NAME):
            with open(os.path.join(self.dir, n), "w") as f:
                f.write("someone else's file\n")
        self.assertFalse(app.is_device(self.dir))

    def test_status_with_a_bom_is_still_recognised(self):
        self.make(self.dir)
        p = os.path.join(self.dir, app.STATUS_NAME)
        data = rb(p)
        with open(p, "wb") as f:
            f.write(b"\xef\xbb\xbf" + data)
        self.assertTrue(app.is_device(self.dir))

    def test_a_missing_folder_is_not_a_device(self):
        self.assertFalse(app.is_device(os.path.join(self.dir, "nope")))

    def test_scanning_real_drives_does_not_crash(self):
        self.assertIsInstance(app.candidate_roots(), list)
        app.find_device(None)


class TestDeviceSync(ServerCase):
    def test_the_first_look_reads_the_taps_in(self):
        s = self.sync()
        self.assertTrue(s["connected"])
        self.assertEqual(s["path"], self.dev)
        self.assertGreater(s["counts"]["taps"], 100)
        self.assertEqual(s["sync"]["seq"], 1)
        self.assertEqual(s["sync"]["new"], s["counts"]["taps"])
        self.assertEqual(s["device"]["device_id"], 12648430)
        self.assertEqual((s["device"]["module"], s["device"]["lecture"]), ("EN2090", "Circuits Lecture 4"))
        self.assertTrue(s["device"]["since"])
        self.assertTrue(s["device"]["last_tap"])
        self.assertIn("now", s)
        self.assertEqual(s["counts"]["students"], 30)

    def test_looking_again_adds_nothing_and_does_not_reread(self):
        a = self.sync()
        b = self.sync()
        self.assertEqual(b["sync"]["seq"], a["sync"]["seq"], "nothing changed on the device, so it was not read again")
        self.assertEqual(b["counts"]["taps"], a["counts"]["taps"])

    def test_new_taps_on_the_device_are_picked_up(self):
        a = self.sync()
        p = os.path.join(self.dev, app.ATTEND_NAME)
        with open(p, "a", newline="") as f:
            f.write("%s,%s,%010d\r\n" % ("2030-01-01", "09:00:00", 1000))
        st = os.path.join(self.dev, app.STATUS_NAME)
        text = rt(st).replace("%d records" % a["device"]["records"], "%d records" % (a["device"]["records"] + 1))
        with open(st, "w", newline="") as f:
            f.write(text)
        b = self.sync()
        self.assertEqual(b["sync"]["seq"], a["sync"]["seq"] + 1)
        self.assertEqual(b["sync"]["new"], 1)
        self.assertEqual(b["counts"]["taps"], a["counts"]["taps"] + 1)

    def test_a_forced_sync_reads_again_but_adds_nothing(self):
        a = self.sync()
        b = self.api("POST", "/api/sync")
        self.assertEqual(b["sync"]["seq"], a["sync"]["seq"] + 1)
        self.assertEqual(b["sync"]["new"], 0)

    def test_unplugging_and_plugging_back_in(self):
        self.sync()
        os.rename(os.path.join(self.dev, app.STATUS_NAME), os.path.join(self.dev, "gone"))
        s = self.sync()
        self.assertFalse(s["connected"])
        self.assertNotIn("device", s)
        self.assertEqual(s["counts"]["students"], 30, "the database is still there")
        self.api("POST", "/api/sync", expect=409)
        os.rename(os.path.join(self.dev, "gone"), os.path.join(self.dev, app.STATUS_NAME))
        self.assertTrue(self.sync()["connected"])

    def test_the_lecture_started_here_is_confirmed_by_the_device_clock(self):
        cur = self.db.current_lecture()
        self.assertEqual(cur["title"], "Circuits Lecture 4")
        self.assertEqual(self.db.con.execute("SELECT confirmed FROM lectures WHERE id=?", (cur["id"],)).fetchone()[0], 0)
        self.sync()
        self.assertEqual(self.db.con.execute("SELECT confirmed FROM lectures WHERE id=?", (cur["id"],)).fetchone()[0], 1)

    def test_a_lecture_started_by_hand_on_the_device_is_adopted_once(self):
        self.db.delete_lecture(self.db.current_lecture()["id"])
        self.sync()
        cur = self.db.current_lecture()
        self.assertIsNotNone(cur)
        self.assertEqual(cur["title"], "Circuits Lecture 4")
        self.db.delete_lecture(cur["id"])
        self.api("POST", "/api/sync")
        self.assertIsNone(self.db.current_lecture(), "a lecture deleted here does not come back on the next read")

    def test_a_corrupt_log_does_not_stop_the_sync(self):
        p = os.path.join(self.dev, app.ATTEND_NAME)
        with open(p, "a", newline="") as f:
            f.write("garbage line\r\n\x00\x00\x00\r\n2026-13-45,99:99:99,5\r\n")
        s = self.api("POST", "/api/sync")
        self.assertEqual(s["sync"]["skipped"], 2)
        self.assertGreater(s["counts"]["taps"], 100)


class TestStudentsApi(ServerCase):
    def test_list_search_and_filters(self):
        r = self.get("/api/students")
        self.assertEqual(len(r["students"]), 30)
        self.assertIn("Electrical Engineering", r["departments"])
        r = self.get("/api/students?q=amal")
        self.assertEqual([s["name"] for s in r["students"]], ["Amal Perera"])
        r = self.get("/api/students?module=CS1010")
        self.assertTrue(r["students"] and all("CS1010" in s["modules"] for s in r["students"]))
        r = self.get("/api/students?department=Computer%20Science")
        self.assertTrue(all(s["department"] == "Computer Science" for s in r["students"]))

    def test_register_edit_and_delete(self):
        s = self.api("POST", "/api/students", {"card_id": "0000424242", "name": "New Student", "student_no": "EN/21/999",
                                                "department": "Electrical Engineering", "modules": ["EN2090", "MA1010"]})
        self.assertEqual((s["card_id"], s["modules"]), (424242, ["EN2090", "MA1010"]))
        s = self.api("POST", "/api/students", {"card_id": 424242, "name": "Renamed", "modules": ["EN2090"]})
        self.assertEqual((s["name"], s["modules"], s["department"]), ("Renamed", ["EN2090"], ""))
        self.api("DELETE", "/api/students/424242")
        self.api("DELETE", "/api/students/424242", expect=400)

    def test_bad_input_gets_a_message_and_changes_nothing(self):
        for body, word in [({"card_id": "", "name": "A"}, "card number"), ({"card_id": "abc", "name": "A"}, "digits"),
                           ({"card_id": 5, "name": ""}, "name"), ({"card_id": 4294967295, "name": "A"}, "large")]:
            r = self.api("POST", "/api/students", body, expect=400)
            self.assertIn(word, r["error"])
        self.assertEqual(len(self.get("/api/students")["students"]), 30)

    def test_registering_an_unregistered_card(self):
        self.sync()
        cards = self.get("/api/cards/unregistered")["cards"]
        self.assertEqual({c["card_id"] for c in cards}, {424242, 535353})
        self.assertTrue(all(c["last"] for c in cards))
        self.api("POST", "/api/students", {"card_id": cards[0]["card_id"], "name": "Newly Enrolled", "modules": ["EN2090"]})
        self.assertEqual(len(self.get("/api/cards/unregistered")["cards"]), 1)
        # and the lecture they tapped at now counts them
        cur = self.db.current_lecture()
        att = self.get("/api/lectures/%d" % cur["id"])
        self.assertIn("Newly Enrolled", [p["name"] for p in att["present"]])

    def test_bulk_import_and_export(self):
        r = self.api("POST", "/api/students/import", {"text": "card_id,name,student_no,department,modules\r\n9001,Imp One,X/1,Maths,MA1010\r\n9002,Imp Two\r\nbad,Bad\r\n"})
        self.assertEqual((r["added"], len(r["errors"])), (2, 1))
        code, data, hdrs = self.raw("GET", "/api/students.csv")
        self.assertEqual(code, 200)
        self.assertIn("students.csv", hdrs["Content-Disposition"])
        self.assertTrue(data.startswith(b"\xef\xbb\xbfcard_id,name"))
        self.assertIn(b"Imp One", data)
        self.api("POST", "/api/students/import", {"nope": 1}, expect=400)

    def test_unicode_survives(self):
        self.api("POST", "/api/students", {"card_id": 777, "name": "සිංහල නම", "department": "தமிழ்"})
        s = [x for x in self.get("/api/students")["students"] if x["card_id"] == 777][0]
        self.assertEqual((s["name"], s["department"]), ("සිංහල නම", "தமிழ்"))


class TestModulesApi(ServerCase):
    def test_modules(self):
        mods = {m["code"]: m for m in self.get("/api/modules")["modules"]}
        self.assertEqual(set(mods), {"EN2090", "MA1010", "CS1010"})
        self.assertEqual(mods["MA1010"]["students"], 30)
        self.api("POST", "/api/modules", {"code": "PH1000", "title": "Physics", "department": "Physics"})
        self.assertEqual(self.db.get_module("PH1000")["title"], "Physics")
        self.api("POST", "/api/modules", {"code": "  "}, expect=400)

    def test_enrolment(self):
        self.api("POST", "/api/modules", {"code": "PH1000"})
        self.api("POST", "/api/modules/PH1000/enroll", {"card_ids": [1000, 1007], "mode": "add"})
        self.assertEqual(len(self.get("/api/students?module=PH1000")["students"]), 2)
        self.api("POST", "/api/modules/PH1000/enroll", {"card_ids": [1000], "mode": "remove"})
        self.assertEqual(len(self.get("/api/students?module=PH1000")["students"]), 1)
        r = self.api("POST", "/api/modules/PH1000/enroll", {"card_ids": [123456789], "mode": "add"}, expect=400)
        self.assertIn("not a registered student", r["error"])

    def test_rename_and_delete(self):
        self.api("POST", "/api/modules/rename", {"old": "CS1010", "new": "CS1011"})
        self.assertIn("CS1011", [m["code"] for m in self.get("/api/modules")["modules"]])
        r = self.api("DELETE", "/api/modules/CS1011", expect=400)
        self.assertIn("lecture", r["error"])
        self.api("DELETE", "/api/modules/CS1011?force=1")
        self.assertNotIn("CS1011", [m["code"] for m in self.get("/api/modules")["modules"]])
        self.api("POST", "/api/modules/rename", {"old": "NOPE", "new": "X"}, expect=400)

    def test_a_module_code_with_a_space_in_the_url(self):
        self.api("POST", "/api/modules", {"code": "EN 2090 B"})
        self.api("POST", "/api/modules/EN%202090%20B/enroll", {"card_ids": [1000], "mode": "add"})
        self.api("DELETE", "/api/modules/EN%202090%20B")


class TestLecturesApi(ServerCase):
    def test_listing_with_counts(self):
        self.sync()
        r = self.get("/api/lectures")
        titles = [l["title"] for l in r["lectures"]]
        self.assertEqual(titles[0], "Circuits Lecture 4", "newest first")
        self.assertEqual(len(titles), 7)
        first = r["lectures"][-1]
        self.assertGreater(first["counts"]["enrolled"], 5)
        self.assertGreater(first["counts"]["present_enrolled"], 0)
        self.assertTrue(r["unassigned"], "the demo has taps taken with no lecture running")
        only = self.get("/api/lectures?module=MA1010")["lectures"]
        self.assertEqual({l["module_code"] for l in only}, {"MA1010"})
        self.assertEqual(self.get("/api/lectures?from=2099-01-01")["lectures"], [])

    def test_attendance_detail(self):
        self.sync()
        lec = [l for l in self.get("/api/lectures")["lectures"] if l["title"] == "Circuits Lecture 1"][0]
        a = self.get("/api/lectures/%d" % lec["id"])
        c = a["counts"]
        self.assertEqual(c["present_enrolled"] + c["absent"], c["enrolled"])
        self.assertEqual(len(a["present"]), c["present_enrolled"])
        self.assertTrue(all(p["name"] and p["time"] for p in a["present"]))
        self.assertTrue(all(not p["enrolled"] or p["enrolled"] for p in a["present"]))
        self.api("GET", "/api/lectures/99999", expect=400)

    def test_downloads(self):
        self.sync()
        lec = [l for l in self.get("/api/lectures")["lectures"] if l["title"] == "Circuits Lecture 1"][0]
        code, data, hdrs = self.raw("GET", "/api/lectures/%d.csv" % lec["id"])
        self.assertEqual(code, 200)
        self.assertIn("attachment", hdrs["Content-Disposition"])
        self.assertIn("Circuits_Lecture_1", hdrs["Content-Disposition"])
        text = data.decode("utf-8-sig")
        self.assertTrue(text.startswith("Module,EN2090"))
        self.assertIn("Present", text)
        self.assertIn("Absent", text)
        code, data, hdrs = self.raw("GET", "/api/lectures/%d.html" % lec["id"])
        self.assertEqual(code, 200)
        page = data.decode("utf-8")
        self.assertIn("<h1>Circuits Lecture 1</h1>", page)
        self.assertIn("Absent (", page)
        self.assertIn("Save as PDF", page)

    def test_pdf(self):
        self.sync()
        lec = self.get("/api/lectures")["lectures"][0]
        code, data, hdrs = self.raw("GET", "/api/lectures/%d.pdf" % lec["id"])
        if R.find_browser():
            self.assertEqual(code, 200, data[:200])
            self.assertTrue(data.startswith(b"%PDF"), "a real PDF")
            self.assertGreater(len(data), 2000)
            self.assertEqual(hdrs["Content-Type"], "application/pdf")
        else:
            self.assertEqual(code, 501)
            self.assertIn("Chrome or Edge", json.loads(data)["error"])

    def test_create_edit_end_delete(self):
        l = self.api("POST", "/api/lectures", {"module": "EN2090", "title": "Make-up lecture", "start": "2026-01-05 10:00", "end": "2026-01-05 11:00"})
        self.assertEqual((l["title"], l["start_text"], l["end_text"]), ("Make-up lecture", "2026-01-05 10:00:00", "2026-01-05 11:00:00"))
        l = self.api("PATCH", "/api/lectures/%d" % l["id"], {"title": "Make-up, moved", "end": "2026-01-05 11:30"})
        self.assertEqual((l["title"], l["end_text"]), ("Make-up, moved", "2026-01-05 11:30:00"))
        l = self.api("PATCH", "/api/lectures/%d" % l["id"], {"end": ""})
        self.assertTrue(l["running"])
        l = self.api("POST", "/api/lectures/%d/end" % l["id"])
        self.assertFalse(l["running"])
        self.api("DELETE", "/api/lectures/%d" % l["id"])
        self.api("DELETE", "/api/lectures/%d" % l["id"], expect=400)
        self.api("POST", "/api/lectures", {"module": "EN2090", "title": "Bad", "start": "2026-01-05 10:00", "end": "2026-01-05 09:00"}, expect=400)
        self.api("POST", "/api/lectures", {"module": "EN2090", "title": "Bad", "start": "not a date"}, expect=400)
        self.api("PATCH", "/api/lectures/99999", {"title": "x"}, expect=400)

    def test_unassigned_taps_become_a_lecture(self):
        self.sync()
        day = self.get("/api/lectures")["unassigned"][0]
        l = self.api("POST", "/api/unassigned/lecture", {"date": day["date"], "module": "MA1010", "title": "Extra tutorial"})
        a = self.get("/api/lectures/%d" % l["id"])
        self.assertGreater(a["counts"]["present_enrolled"], 0)
        self.assertNotIn(day["date"], [d["date"] for d in self.get("/api/lectures")["unassigned"]])
        self.api("POST", "/api/unassigned/lecture", {"date": day["date"], "module": "MA1010", "title": "Again"}, expect=400)


class TestStartLecture(ServerCase):
    def test_sends_the_lecture_to_the_device_and_records_it_here(self):
        self.sync()
        r = self.api("POST", "/api/lectures/start", {"module": "MA1010", "title": "Calculus Lecture 3"})
        self.assertTrue(r["clock_set"])
        text = self.settings_text()
        self.assertRegex(text, r"\r\n#TIME,\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\r\n")
        self.assertIn("\r\n#MODULE,MA1010\r\n", text)
        self.assertIn("\r\n#LECTURE,Calculus Lecture 3\r\n", text)
        self.assertIn("\r\n#NEWSESSION,1\r\n", text)
        self.assertIn("\r\n#DEVICE,0012648430\r\n", text, "the device number is kept")
        self.assertNotIn("CARD_ID", text, "no student data goes to the device")
        cur = self.db.current_lecture()
        self.assertEqual((cur["module_code"], cur["title"]), ("MA1010", "Calculus Lecture 3"))
        self.assertAlmostEqual(cur["start_ts"], D.now_ts(), delta=5)
        prev = [l for l in self.db.list_lectures() if l["title"] == "Circuits Lecture 4"][0]
        self.assertFalse(prev["running"], "starting a lecture ends the one before")

    def test_without_the_clock_the_time_line_is_left_out(self):
        self.sync()
        self.api("POST", "/api/lectures/start", {"module": "MA1010", "title": "No clock", "sync_clock": False})
        text = self.settings_text()
        self.assertNotIn("\r\n#TIME,", text)
        self.assertIn("\r\n#LECTURE,No clock\r\n", text)

    def test_the_start_time_follows_the_devices_clock_when_it_is_not_reset(self):
        self.sync()
        self.app.pc_minus_device = 600             # the PC is ten minutes ahead of the device
        before = D.now_ts()
        self.api("POST", "/api/lectures/start", {"module": "MA1010", "title": "Offset", "sync_clock": False})
        cur = self.db.current_lecture()
        self.assertAlmostEqual(cur["start_ts"], before - 600, delta=5, msg="in the device's time, not the PC's")

    def test_names_are_made_safe_for_the_device(self):
        self.sync()
        self.api("POST", "/api/lectures/start", {"module": "EN,2090", "title": 'Part "1", intro ' + "x" * 40})
        text = self.settings_text()
        self.assertIn("\r\n#MODULE,EN 2090\r\n", text)
        line = [l for l in text.split("\r\n") if l.startswith("#LECTURE,")][0]
        self.assertLessEqual(len(line[9:].encode("utf-8")), 32)
        self.assertNotIn('"', line)

    def test_validation_and_no_device(self):
        self.sync()
        self.api("POST", "/api/lectures/start", {"module": "", "title": "L"}, expect=400)
        self.api("POST", "/api/lectures/start", {"module": "M", "title": ""}, expect=400)
        n = self.db.counts()["lectures"]
        before = self.settings_text()
        os.rename(os.path.join(self.dev, app.STATUS_NAME), os.path.join(self.dev, "gone"))
        r = self.api("POST", "/api/lectures/start", {"module": "MA1010", "title": "Nowhere"}, expect=409)
        self.assertIn("not connected", r["error"])
        self.assertEqual(self.db.counts()["lectures"], n, "nothing is recorded when the device could not be written")
        os.rename(os.path.join(self.dev, "gone"), os.path.join(self.dev, app.STATUS_NAME))
        self.assertEqual(self.settings_text(), before)

    def test_setting_the_clock_alone(self):
        self.sync()
        self.api("POST", "/api/device/clock", {})
        text = self.settings_text()
        self.assertRegex(text, r"\r\n#TIME,\d{4}")
        self.assertNotIn("\r\n#MODULE,", text)
        self.assertNotIn("\r\n#NEWSESSION", text, "setting the clock must not start a lecture")

    def test_setting_the_device_number(self):
        self.sync()
        self.api("POST", "/api/device/id", {"device_id": 77})
        self.assertIn("\r\n#DEVICE,0000000077\r\n", self.settings_text())
        for bad in (-1, 4294967295, "abc", None):
            self.api("POST", "/api/device/id", {"device_id": bad}, expect=400)


class TestCardList(ServerCase):
    """The device compares taps with a list of card numbers (no names), so it can show green or red."""

    def cards_in(self, text):
        lines = text.split("\r\n")
        i = [n for n, l in enumerate(lines) if l.startswith("#CARDS,")][0]
        count = int(lines[i][7:])
        return count, [int(l) for l in lines[i + 1:i + 1 + count]]

    def test_the_demo_device_already_knows_the_students(self):
        c = self.sync()["cards"]
        self.assertEqual((c["device"], c["database"], c["in_sync"]), (30, 30, True))

    def test_a_new_student_makes_the_device_out_of_date_until_the_list_is_sent(self):
        self.sync()
        self.api("POST", "/api/students", {"card_id": 424242, "name": "New Student", "modules": ["MA1010"]})
        c = self.sync()["cards"]
        self.assertEqual((c["device"], c["database"], c["in_sync"]), (30, 31, False))
        self.assertEqual(self.api("POST", "/api/device/cards", {})["count"], 31)
        text = self.settings_text()
        n, ids = self.cards_in(text)
        self.assertEqual((n, ids), (31, sorted(self.db.card_ids())))
        self.assertIn(424242, ids)
        self.assertNotIn("New Student", text, "names stay on the PC")
        self.assertNotIn("\r\n#MODULE,", text, "sending the cards must not touch the lecture")
        self.assertNotIn("\r\n#NEWSESSION", text)

    def test_the_same_number_of_different_cards_is_still_out_of_date(self):
        self.sync()
        first = self.db.card_ids()[0]
        self.api("DELETE", "/api/students/%d" % first)
        self.api("POST", "/api/students", {"card_id": 424242, "name": "Swap"})
        c = self.sync()["cards"]
        self.assertEqual((c["device"], c["database"], c["in_sync"]), (30, 30, False), "the CRC tells them apart")

    def test_starting_a_lecture_sends_the_list_too(self):
        self.sync()
        self.api("POST", "/api/lectures/start", {"module": "MA1010", "title": "With cards"})
        n, ids = self.cards_in(self.settings_text())
        self.assertEqual(ids, sorted(self.db.card_ids()))
        self.assertEqual(ids, sorted(set(ids)), "ascending, no repeats")

    def test_the_clock_and_the_device_number(self):
        self.sync()
        self.api("POST", "/api/device/clock", {})
        self.assertEqual(self.cards_in(self.settings_text())[0], 30)
        self.api("POST", "/api/device/id", {"device_id": 77})
        self.assertNotIn("#CARDS", self.settings_text(), "changing the number leaves the list alone")

    def test_more_students_than_the_device_holds(self):
        self.sync()
        for i in range(D.DEVICE_CARDS_MAX + 1 - 30):
            self.db.upsert_student(100000 + i, "Extra %d" % i)
        c = self.sync()["cards"]
        self.assertTrue(c["too_many"])
        before = self.settings_text()
        r = self.api("POST", "/api/device/cards", {}, expect=400)
        self.assertIn("1000", r["error"])
        self.api("POST", "/api/lectures/start", {"module": "MA1010", "title": "Too many"}, expect=400)
        self.assertEqual(self.settings_text(), before, "nothing was written")
        self.assertEqual([l for l in self.db.list_lectures() if l["title"] == "Too many"], [])

    def test_an_empty_database_clears_the_device_list(self):
        self.sync()
        for cid in self.db.card_ids():
            self.api("DELETE", "/api/students/%d" % cid)
        self.api("POST", "/api/device/cards", {})
        self.assertIn("\r\n#CARDS,0\r\n", self.settings_text())


class TestReportsApi(ServerCase):
    def setUp(self):
        super().setUp()
        self.sync()

    def test_module_report(self):
        r = self.get("/api/reports/module/EN2090")
        self.assertEqual(r["module"]["code"], "EN2090")
        self.assertEqual(r["summary"]["lectures"], 4)
        self.assertTrue(r["students"] and all(len(s["flags"]) == 4 for s in r["students"]))
        r = self.get("/api/reports/module/EN2090?from=2099-01-01")
        self.assertEqual(r["summary"]["lectures"], 0)
        self.api("GET", "/api/reports/module/NOPE", expect=400)

    def test_module_exports(self):
        code, data, hdrs = self.raw("GET", "/api/reports/module/EN2090.csv")
        self.assertEqual(code, 200)
        text = data.decode("utf-8-sig")
        self.assertTrue(text.startswith("CARD_ID,NAME,STUDENT_NO,DEPARTMENT"))
        self.assertIn("EN2090_attendance.csv", hdrs["Content-Disposition"])
        code, data, _ = self.raw("GET", "/api/reports/module/EN2090.html")
        self.assertEqual(code, 200)
        self.assertIn("Circuits and Systems", data.decode("utf-8"))
        code, data, _ = self.raw("GET", "/api/reports/module/EN2090.pdf")
        self.assertEqual(code, 200 if R.find_browser() else 501)
        if code == 200:
            self.assertTrue(data.startswith(b"%PDF"))

    def test_student_report(self):
        r = self.get("/api/reports/student/1000")
        self.assertEqual(r["student"]["name"], "Amal Perera")
        self.assertGreater(r["taps"], 0)
        self.assertEqual({m["code"] for m in r["modules"]}, {"MA1010", "EN2090"})
        code, data, hdrs = self.raw("GET", "/api/reports/student/1000.csv")
        self.assertEqual(code, 200)
        self.assertIn("Amal_Perera_attendance.csv", hdrs["Content-Disposition"])
        code, data, _ = self.raw("GET", "/api/reports/student/1000.html")
        self.assertIn("Amal Perera", data.decode("utf-8"))
        self.api("GET", "/api/reports/student/424242", expect=400)

    def test_report_pages_escape_what_they_show(self):
        self.db.upsert_student(31337, "<script>alert(1)</script> & Co", "A&B", "R<D", ["EN2090"])
        code, data, _ = self.raw("GET", "/api/reports/student/31337.html")
        page = data.decode("utf-8")
        self.assertNotIn("<script>alert", page)
        self.assertIn("&lt;script&gt;", page)
        code, data, _ = self.raw("GET", "/api/reports/module/EN2090.html")
        self.assertNotIn("<script>alert", data.decode("utf-8"))


class TestTapsApi(ServerCase):
    def test_import_a_saved_file(self):
        text = "DATE,TIME,CARD_ID\r\n2027-01-01,09:00:00,0000001000\r\n2027-01-01,09:00:05,0000001007\r\n"
        r = self.api("POST", "/api/taps/import", {"text": text})
        self.assertEqual((r["read"], r["new"]), (2, 2))
        r = self.api("POST", "/api/taps/import", {"text": text})
        self.assertEqual((r["read"], r["new"]), (2, 0), "importing the same file twice adds nothing")
        r = self.api("POST", "/api/taps/import", {"text": "this is not an attendance file"}, expect=400)
        self.assertIn("No taps", r["error"])
        self.api("POST", "/api/taps/import", {"text": 5}, expect=400)

    def test_listing(self):
        self.sync()
        taps = self.get("/api/taps?card=1000&limit=5")["taps"]
        self.assertEqual(len(taps), 5)
        self.assertTrue(all(t["card_id"] == 1000 and t["name"] == "Amal Perera" for t in taps))
        self.assertEqual(self.get("/api/taps?from=2099-01-01")["taps"], [])
        self.api("GET", "/api/taps?card=abc", expect=400)
        code, data, _ = self.raw("GET", "/api/taps.csv")
        self.assertEqual(code, 200)
        self.assertTrue(data.decode("utf-8-sig").startswith("date,time,card_id,name"))

    def test_backup(self):
        code, data, hdrs = self.raw("GET", "/api/backup.db")
        self.assertEqual(code, 200)
        self.assertTrue(data.startswith(b"SQLite format 3"))
        p = os.path.join(self.data, "restored.db")
        with open(p, "wb") as f:
            f.write(data)
        con = sqlite3.connect(p)
        self.assertEqual(con.execute("SELECT COUNT(*) FROM students").fetchone()[0], 30)
        con.close()


class TestSafety(ServerCase):
    def test_ping_and_static(self):
        self.assertEqual(self.get("/api/ping")["app"], "attendance-companion")
        code, body, _ = self.raw("GET", "/")
        self.assertEqual(code, 200)
        self.assertIn(b"<html", body.lower())
        for bad in ("/../attendance_app.py", "/..%2fattendance_app.py", "/%2e%2e/attendance_app.py", "/nothing.html"):
            self.assertEqual(self.raw("GET", bad)[0], 404, bad)

    def test_unknown_routes(self):
        self.api("GET", "/api/nothing", expect=404)
        self.api("POST", "/api/nothing", {}, expect=404)
        self.api("PUT", "/api/students", {}, expect=404) if False else None

    def test_changes_need_the_header_and_a_local_origin(self):
        for method in ("POST", "PATCH", "DELETE"):
            path = {"POST": "/api/students", "PATCH": "/api/lectures/1", "DELETE": "/api/students/1000"}[method]
            code, _, _ = self.raw(method, path, {"card_id": 5, "name": "x"}, headers={"X-Attendance": "0"})
            self.assertEqual(code, 403, method)
        code, _, _ = self.raw("POST", "/api/students", {"card_id": 5, "name": "x"}, headers={"Origin": "http://evil.example"})
        self.assertEqual(code, 403, "a page from another site")
        code, _, _ = self.raw("POST", "/api/students", {"card_id": 5, "name": "x"}, host="evil.example")
        self.assertEqual(code, 403, "DNS rebinding: wrong Host")
        code, _, _ = self.raw("GET", "/api/state", host="evil.example")
        self.assertEqual(code, 403)
        code, _, _ = self.raw("GET", "/api/students.csv", host="evil.example")
        self.assertEqual(code, 403, "downloads too")
        self.assertIsNone(self.db.get_student(5), "none of that wrote anything")
        self.assertEqual(self.db.counts()["students"], 30)

    def test_origin_of_this_app_is_allowed(self):
        code, _, _ = self.raw("POST", "/api/students", {"card_id": 5, "name": "x"},
                              headers={"Origin": "http://127.0.0.1:%d" % self.port})
        self.assertEqual(code, 200)

    def test_bad_bodies(self):
        h = {"X-Attendance": "1"}
        self.assertEqual(self.raw("POST", "/api/students", b"not json", headers=h)[0], 400)
        self.assertEqual(self.raw("POST", "/api/students", b"[1,2]", headers=h)[0], 400)
        self.assertEqual(self.raw("POST", "/api/students", {"name": "no card"}, headers=h)[0], 400)
        self.assertEqual(self.raw("POST", "/api/students", b"x" * (app.MAX_BODY + 10), headers=h)[0], 413)
        self.assertEqual(self.db.counts()["students"], 30)

    def test_the_largest_pasted_class_list_is_accepted(self):
        lines = "\r\n".join("%d,Student %d,S%d,Dept,EN2090" % (50000 + i, i, i) for i in range(2000))
        r = self.api("POST", "/api/students/import", {"text": lines})
        self.assertEqual(r["added"], 2000)

    def test_quit_stops_the_server(self):
        self.assertEqual(self.raw("POST", "/api/quit", {})[0], 200)
        self.thread.join(timeout=5)
        self.assertFalse(self.thread.is_alive())
        self.server.server_close()
        self.server.shutdown = lambda: None


class TestBackups(unittest.TestCase):
    def test_one_dated_copy_a_day_and_old_ones_go(self):
        d = tempfile.mkdtemp(prefix="att-bak-")
        try:
            db = D.Database(os.path.join(d, "a.db"))
            self.assertIsNone(None if not os.path.exists(os.path.join(d, "backups")) else 1)
            app.rotate_backup(db, d)                                  # nothing to save yet
            self.assertEqual(os.listdir(os.path.join(d, "backups")), [])
            db.upsert_student(5, "Kept")
            p = app.rotate_backup(db, d)
            self.assertTrue(os.path.isfile(p))
            self.assertTrue(rb(p).startswith(b"SQLite format 3"))
            m = os.path.getmtime(p)
            app.rotate_backup(db, d)
            self.assertEqual(os.path.getmtime(p), m, "a second start the same day does not overwrite it")
            folder = os.path.join(d, "backups")
            for i in range(30):
                open(os.path.join(folder, "attendance-2020%02d%02d.db" % (1 + i // 28, 1 + i % 28)), "wb").close()
            app.rotate_backup(db, d)
            kept = sorted(os.listdir(folder))
            self.assertEqual(len(kept), app.BACKUPS_KEPT)
            self.assertIn(os.path.basename(p), kept, "the newest is always kept")
            db.close()
        finally:
            shutil.rmtree(d, ignore_errors=True)


class TestDemo(unittest.TestCase):
    def test_demo_files_have_the_firmwares_shape(self):
        data, dev = tempfile.mkdtemp(prefix="att-demo-d-"), tempfile.mkdtemp(prefix="att-demo-v-")
        try:
            app.make_demo(data, dev)
            raw = rb(os.path.join(dev, app.ATTEND_NAME))
            self.assertEqual(len(raw) % 32, 0, "every row is 32 bytes")
            for i in range(0, len(raw), 32):
                self.assertEqual(raw[i + 30:i + 32], b"\r\n")
            self.assertEqual(len(rb(os.path.join(dev, app.STATUS_NAME))), 512)
            rows, skipped = D.parse_attend(raw.decode())
            self.assertEqual(skipped, 0)
            self.assertEqual(len(rows), len(raw) // 32 - 1)
            st = D.parse_status(rb(os.path.join(dev, app.STATUS_NAME)).decode())
            self.assertTrue(st["ok"] and st["has_lecture"] and st["since"])
            self.assertEqual(st["records"], len(rows))
        finally:
            shutil.rmtree(data, ignore_errors=True)
            shutil.rmtree(dev, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()

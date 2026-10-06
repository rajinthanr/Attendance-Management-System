"""Tests for attendance_db.py.   Run from the Companion folder:  python -m unittest discover -s tests -v"""
import csv
import io
import os
import sqlite3
import sys
import tempfile
import threading
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import attendance_db as D  # noqa: E402

T0 = D.parse_ts("2026-10-06 09:00:00")      # a convenient lecture start


def hms(seconds):
    return T0 + seconds


class TextAndTimes(unittest.TestCase):
    def test_card_numbers(self):
        self.assertEqual(D.parse_card("123456"), 123456)
        self.assertEqual(D.parse_card("0000123456"), 123456)
        self.assertEqual(D.parse_card(" 42 "), 42)
        self.assertEqual(D.parse_card("0x1E240"), 123456)
        self.assertEqual(D.parse_card("0XabC"), 0xABC)
        self.assertEqual(D.parse_card(4294967039), 4294967039)
        for bad in ["", "  ", "0", "0x0", "-5", "12ab", "abc", "1.5", "4294967040", "4294967295", "99999999999", "1 2", None]:
            with self.assertRaises(D.DbError, msg=repr(bad)):
                D.parse_card(bad)
        self.assertEqual(D.fmt_card(42), "0000000042")
        self.assertEqual(D.fmt_card(4294967039), "4294967039")

    def test_device_text_matches_the_firmware(self):
        self.assertEqual(D.clean_device_text("  Alice  ", 27), "Alice")
        self.assertEqual(D.clean_device_text("EN,2090", 24), "EN 2090")
        self.assertEqual(D.clean_device_text('say "hi"', 32), "say  hi")
        self.assertEqual(D.clean_device_text("a\tb\nc", 32), "a b c")
        self.assertEqual(D.clean_device_text("x" * 40, 24), "x" * 24)
        self.assertEqual(D.clean_device_text("a" * 23 + "é", 24), "a" * 23)         # é would be bytes 24-25
        self.assertEqual(D.clean_device_text("a" * 22 + "é", 24), "a" * 22 + "é")
        self.assertEqual(D.clean_device_text("a" * 23 + " b", 24), "a" * 23)         # no trailing space after the cut
        self.assertEqual(D.clean_device_text(None, 24), "")
        self.assertEqual(D.cut_utf8("😀😀", 5), "😀")

    def test_free_text(self):
        self.assertEqual(D.clean_free_text("  Smith,   John \n"), "Smith, John")
        self.assertEqual(D.clean_free_text("x" * 200, 80), "x" * 80)
        self.assertEqual(D.clean_free_text(None), "")

    def test_times_round_trip_and_have_no_zone(self):
        for text in ["2026-10-06 09:30:00", "2000-01-01 00:00:00", "2099-12-31 23:59:59", "2024-02-29 12:00:00"]:
            self.assertEqual(D.fmt_ts(D.parse_ts(text)), text)
        self.assertEqual(D.parse_ts("2026-10-06"), D.parse_ts("2026-10-06 00:00:00"))
        self.assertEqual(D.parse_ts("2026-10-06T09:30"), D.parse_ts("2026-10-06 09:30:00"))
        self.assertEqual(D.parse_ts("2026-1-6 9:05"), D.parse_ts("2026-01-06 09:05:00"))
        # One day is always 86400 s here, even across a daylight-saving change somewhere.
        self.assertEqual(D.parse_ts("2026-03-30") - D.parse_ts("2026-03-29"), 86400)
        self.assertEqual(D.parse_ts("2026-10-26") - D.parse_ts("2026-10-25"), 86400)
        for bad in ["", "yesterday", "2026-13-01", "2026-02-30", "2026-10-06 25:00", "06/10/2026"]:
            with self.assertRaises(D.DbError, msg=bad):
                D.parse_ts(bad)
        self.assertEqual(D.fmt_date(T0), "2026-10-06")


class DeviceFiles(unittest.TestCase):
    def test_attend_rows(self):
        text = ("DATE,TIME,CARD_ID                \r\n2026-10-06,09:30:12,0000123456\r\n"
                "2026-10-06,09:30:40,0000000042\r\n")
        rows, skipped = D.parse_attend(text)
        self.assertEqual(rows, [(D.parse_ts("2026-10-06 09:30:12"), 123456), (D.parse_ts("2026-10-06 09:30:40"), 42)])
        self.assertEqual(skipped, 0)

    def test_attend_tolerates_padding_bom_blanks_and_junk(self):
        text = "﻿DATE,TIME,CARD_ID" + " " * 20 + "\r\n2026-10-06,09:30:12,0000000007\r\n\r\n\x00\x00\x00\x00\r\ngarbage\r\n2026-13-45,99:99:99,5\r\n2026-10-06,09:31:00,0000000000\r\n"
        rows, skipped = D.parse_attend(text)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0][1], 7)
        self.assertEqual(skipped, 3)             # garbage, an impossible date, and card 0
        self.assertEqual(D.parse_attend(""), ([], 0))
        self.assertEqual(D.parse_attend("2026-10-06,09:30:12,4294967295")[0][0][1], 4294967295)   # a marker id would be skipped by the firmware, not here
        rows, _ = D.parse_attend("2026-10-06,09:30:12,0000000007,extra,columns\n")
        self.assertEqual(len(rows), 1)
        rows, _ = D.parse_attend("2026-10-06,09:30:12,0000000007\n2026-10-06,09:30:12,0000000007\n")
        self.assertEqual(len(rows), 2)           # duplicates are the database's business

    STATUS = ("ATTENDANCE LOGGER\r\nDevice ID    : 0012648430\r\nClock        : 2026-10-06 09:30:15\r\n"
              "Attendance   : 60 records in ATTEND.CSV\r\nLast tap     : 0000001007 at 2026-10-06 09:29:58\r\n"
              "Lecture      : EN2090 / Circuits / Lecture 4 (since 2026-10-06 08:00:03)\r\n"
              "SETTINGS.CSV : unchanged\r\n" + " " * 100 + "\r\n")

    def test_status(self):
        s = D.parse_status(self.STATUS)
        self.assertTrue(s["ok"])
        self.assertEqual(s["device_id"], 12648430)
        self.assertEqual(s["clock"], D.parse_ts("2026-10-06 09:30:15"))
        self.assertEqual(s["records"], 60)
        self.assertEqual(s["last_card"], 1007)
        self.assertEqual(s["last_tap"], D.parse_ts("2026-10-06 09:29:58"))
        self.assertEqual((s["module"], s["lecture"]), ("EN2090", "Circuits / Lecture 4"))
        self.assertEqual(s["since"], D.parse_ts("2026-10-06 08:00:03"))
        self.assertTrue(s["has_lecture"])
        self.assertFalse(s["pending"])
        self.assertEqual(s["error"], "")

    def test_status_variants(self):
        s = D.parse_status("ATTENDANCE LOGGER\r\nLecture      : none set\r\nLast tap     : none yet\r\n"
                           "SETTINGS.CSV : OK, will be applied when you unplug the cable; clock will be set\r\n")
        self.assertFalse(s["has_lecture"])
        self.assertIsNone(s["last_card"])
        self.assertTrue(s["pending"])
        s = D.parse_status("ATTENDANCE LOGGER\r\nSETTINGS.CSV : ERROR, the file is empty. Nothing will be applied\r\n")
        self.assertTrue(s["error"].startswith("ERROR"))
        self.assertFalse(s["pending"])
        s = D.parse_status("ATTENDANCE LOGGER\r\nLecture      : EN2090 / Old firmware, no since\r\n")
        self.assertTrue(s["has_lecture"] and s["since"] is None and s["lecture"] == "Old firmware, no since")
        self.assertFalse(D.parse_status("some other file")["ok"])
        self.assertFalse(D.parse_status("")["ok"])
        self.assertTrue(D.parse_status("﻿ATTENDANCE LOGGER\r\n")["ok"])

    def test_building_settings(self):
        t = D.build_settings(now=D.parse_ts("2026-10-06 14:05:09"), module="EN,2090", lecture='Part "1"', new_session=True, device_id=42)
        lines = t.split("\r\n")
        self.assertTrue(lines[0].startswith("# Edit these lines"))
        self.assertEqual(lines[1:6], ["#TIME,2026-10-06 14:05:09", "#MODULE,EN 2090", "#LECTURE,Part  1", "#NEWSESSION,1", "#DEVICE,0000000042"])
        self.assertTrue(t.endswith("\r\n"))
        t = D.build_settings(echo_time="2026-10-06 09:30:00")
        self.assertIn("#TIME,2026-10-06 09:30:00\r\n", t)
        self.assertNotIn("\r\n#MODULE,", t)
        self.assertNotIn("\r\n#NEWSESSION", t)       # (the help line mentions it, so look for a directive line)
        t = D.build_settings(module="", lecture="")
        self.assertIn("#MODULE,\r\n#LECTURE,\r\n", t)
        self.assertNotIn("\r\n#TIME,", D.build_settings())


class CardList(unittest.TestCase):
    def test_status_reports_the_cards_the_device_holds(self):
        s = D.parse_status("ATTENDANCE LOGGER\r\nCards        : 30 registered (CRC 1A2B3C4D)\r\n")
        self.assertEqual((s["cards"], s["cards_crc"]), (30, 0x1A2B3C4D))
        s = D.parse_status("ATTENDANCE LOGGER\r\nCards        : none registered\r\n")
        self.assertEqual((s["cards"], s["cards_crc"]), (0, 0))
        s = D.parse_status("ATTENDANCE LOGGER\r\nCards        : 5 registered\r\n")
        self.assertEqual((s["cards"], s["cards_crc"]), (5, None))
        self.assertIsNone(D.parse_status("ATTENDANCE LOGGER\r\n")["cards"], "an older device does not say")

    def test_the_crc_is_the_standard_one_over_little_endian_words(self):
        self.assertEqual(D.cards_crc([]), 0)
        self.assertEqual(D.cards_crc([1]), 0x99F8B879, "CRC-32 of 01 00 00 00")
        self.assertEqual(D.cards_crc([2, 1, 1]), D.cards_crc([1, 2]), "order and repeats do not matter")
        self.assertNotEqual(D.cards_crc([1, 2]), D.cards_crc([1, 3]))

    def test_building_a_list(self):
        t = D.build_settings(cards=[300, 5, 5, 4000000000])
        self.assertIn("\r\n#CARDS,3\r\n0000000005\r\n0000000300\r\n4000000000\r\n", t)
        self.assertTrue(t.endswith("\r\n"))
        self.assertNotIn("#CARDS", D.build_settings(), "no list: the device keeps its own")
        self.assertIn("\r\n#CARDS,0\r\n", D.build_settings(cards=[]), "an empty list clears it")
        self.assertEqual(D.build_settings(cards=range(1, 1001)).count("\r\n0000"), 1000)
        with self.assertRaises(D.DbError):
            D.build_settings(cards=range(1, 1002))
        with self.assertRaises(D.DbError):
            D.build_settings(cards=[0])
        with self.assertRaises(D.DbError):
            D.build_settings(cards=[0xFFFFFF00])


class Base(unittest.TestCase):
    def setUp(self):
        self.db = D.Database(":memory:")

    def tearDown(self):
        self.db.close()

    def students(self, n=5, module="EN2090"):
        self.db.upsert_module(module, "Circuits")
        for i in range(n):
            self.db.upsert_student(1000 + i, "Student %d" % i, "S%03d" % i, "Electrical", [module])


class Students(Base):
    def test_create_read_update_delete(self):
        s = self.db.upsert_student("0000001234", "  Alice   Perera ", "EN/20/001", "Electrical", ["EN2090", "MA1010"])
        self.assertEqual(s["card_id"], 1234)
        self.assertEqual(s["name"], "Alice Perera")
        self.assertEqual(s["modules"], ["EN2090", "MA1010"])
        self.assertEqual(self.db.get_module("MA1010")["title"], "", "a module named in an enrolment is created")
        s = self.db.upsert_student(1234, "Alice P.", "EN/20/001", "Mechanical", ["EN2090"])
        self.assertEqual((s["name"], s["department"], s["modules"]), ("Alice P.", "Mechanical", ["EN2090"]))
        s = self.db.upsert_student(1234, "Alice P.", modules=None)
        self.assertEqual(s["modules"], ["EN2090"], "None leaves enrolments alone")
        self.assertEqual(s["student_no"], "", "but the other fields are replaced")
        s = self.db.upsert_student(1234, "Alice P.", modules=[])
        self.assertEqual(s["modules"], [], "an empty list clears them")
        self.db.delete_student(1234)
        self.assertIsNone(self.db.get_student(1234))
        with self.assertRaises(D.DbError):
            self.db.delete_student(1234)

    def test_validation(self):
        for card, name in [("", "A"), ("abc", "A"), (0, "A"), (4294967295, "A"), (5, ""), (5, "   "), (5, None)]:
            with self.assertRaises(D.DbError, msg=repr((card, name))):
                self.db.upsert_student(card, name)
        self.assertEqual(self.db.counts()["students"], 0)

    def test_names_keep_their_commas_and_unicode(self):
        s = self.db.upsert_student(5, "Smith, John", department="සිවිල්")
        self.assertEqual(s["name"], "Smith, John")
        self.assertEqual(s["department"], "සිවිල්")
        self.assertEqual(self.db.upsert_student(6, "José Núñez 😀")["name"], "José Núñez 😀")

    def test_search_and_filters(self):
        self.db.upsert_student(1000, "Amal Perera", "EN/1", "Electrical", ["EN2090"])
        self.db.upsert_student(1001, "Nimali Silva", "CS/2", "Computing", ["CS1010"])
        self.db.upsert_student(1002, "Kasun Perera", "EN/3", "Electrical", ["EN2090", "CS1010"])
        names = lambda **kw: [s["name"] for s in self.db.list_students(**kw)]
        self.assertEqual(names(), ["Amal Perera", "Kasun Perera", "Nimali Silva"])
        self.assertEqual(names(q="perera"), ["Amal Perera", "Kasun Perera"])
        self.assertEqual(names(q="CS/2"), ["Nimali Silva"])
        self.assertEqual(names(q="1002"), ["Kasun Perera"])
        self.assertEqual(names(q="0000001001"), ["Nimali Silva"])
        self.assertEqual(names(q="computing"), ["Nimali Silva"])
        self.assertEqual(names(module="CS1010"), ["Kasun Perera", "Nimali Silva"])
        self.assertEqual(names(department="Electrical"), ["Amal Perera", "Kasun Perera"])
        self.assertEqual(names(q="perera", module="CS1010"), ["Kasun Perera"])
        self.assertEqual(names(q="no such"), [])
        self.assertEqual(names(q="Perera%Silva"), [], "a % typed by the user is not a wildcard")
        self.assertEqual(names(q="Per_ra"), [], "nor is _")
        self.assertEqual(self.db.departments(), ["Computing", "Electrical"])

    def test_import_with_header_in_any_order(self):
        text = "Name,Card ID,Dept,Student No,Modules\r\nAlice,1001,Electrical,EN/1,EN2090;MA1010\r\nBob,0x3EA,,,\r\n"
        r = self.db.import_students(text)
        self.assertEqual((r["added"], r["updated"], r["errors"]), (2, 0, []))
        self.assertEqual(self.db.get_student(1001)["modules"], ["EN2090", "MA1010"])
        self.assertEqual(self.db.get_student(1001)["student_no"], "EN/1")
        self.assertEqual(self.db.get_student(0x3EA)["name"], "Bob")
        r = self.db.import_students("card_id,name\r\n1001,Alice Renamed\r\n")
        self.assertEqual((r["added"], r["updated"]), (0, 1))
        self.assertEqual(self.db.get_student(1001)["modules"], ["EN2090", "MA1010"], "no modules column: enrolments stay")

    def test_import_without_header_and_from_excel(self):
        r = self.db.import_students("2001\tAlice\tEN/1\tElectrical\tEN2090\n2002\tBob\n")
        self.assertEqual((r["added"], r["errors"]), (2, []))
        self.assertEqual(self.db.get_student(2001)["department"], "Electrical")
        self.assertEqual(self.db.get_student(2002)["modules"], [])
        r = self.db.import_students("﻿3001,Eve\r\n")
        self.assertEqual(r["added"], 1)

    def test_import_reports_bad_lines_and_keeps_the_good(self):
        r = self.db.import_students("card_id,name\r\n1,Good\r\nabc,Bad card\r\n3,\r\n4,Fine\r\n1,Dup of the first\r\n")
        self.assertEqual(r["added"], 2)
        self.assertEqual(r["updated"], 1)
        self.assertEqual([e["line"] for e in r["errors"]], [3, 4])
        self.assertEqual(self.db.get_student(1)["name"], "Dup of the first")
        self.assertEqual(self.db.import_students("")["added"], 0)
        with self.assertRaises(D.DbError):
            self.db.import_students("card_id,foo\r\n1,2\r\n")      # a header that names no name column
        r = self.db.import_students("foo,bar\r\n1,2\r\n")           # no recognisable header: every line is data
        self.assertEqual((r["added"] + r["updated"], [e["line"] for e in r["errors"]]), (1, [1]))   # (card 1 exists by now: an update)

    def test_export_round_trips(self):
        self.db.upsert_student(5, "Smith, John", "EN/5", "Electrical", ["EN2090", "MA1010"])
        self.db.upsert_student(6, "Quote \"Q\"", "", "", [])
        text = self.db.students_csv()
        other = D.Database(":memory:")
        r = other.import_students(text)
        self.assertEqual((r["added"], r["errors"]), (2, []))
        self.assertEqual(other.get_student(5), {**self.db.get_student(5), "created_at": other.get_student(5)["created_at"],
                                                "updated_at": other.get_student(5)["updated_at"]})
        self.assertEqual(other.get_student(6)["name"], 'Quote "Q"')
        other.close()


class Modules(Base):
    def test_modules_and_enrolment(self):
        self.db.upsert_module("EN2090", "Circuits", "Electrical")
        self.db.upsert_module("EN2090", "Circuits and Systems", "Electrical")
        self.assertEqual(self.db.get_module("EN2090")["title"], "Circuits and Systems")
        for i in range(4):
            self.db.upsert_student(10 + i, "S%d" % i)
        self.db.set_enrollment("EN2090", [10, 11, 12], "add")
        self.assertEqual(self.db.list_modules()[0]["students"], 3)
        self.db.set_enrollment("EN2090", [11], "remove")
        self.assertEqual(self.db.get_student(11)["modules"], [])
        self.db.set_enrollment("EN2090", [13], "set")
        self.assertEqual(self.db.list_students(module="EN2090")[0]["card_id"], 13)
        self.assertEqual(len(self.db.list_students(module="EN2090")), 1)
        with self.assertRaises(D.DbError):
            self.db.set_enrollment("EN2090", [999], "add")        # not a student
        with self.assertRaises(D.DbError):
            self.db.set_enrollment("NOPE", [10], "add")
        with self.assertRaises(D.DbError):
            self.db.set_enrollment("EN2090", [10], "explode")
        self.assertEqual(len(self.db.list_students(module="EN2090")), 1, "a refused change changes nothing")

    def test_module_codes_are_device_safe(self):
        self.assertEqual(self.db.upsert_module("EN,2090")["code"], "EN 2090")
        self.assertEqual(self.db.upsert_module("x" * 40)["code"], "x" * 24)
        with self.assertRaises(D.DbError):
            self.db.upsert_module("   ")

    def test_rename_follows_enrolments_and_lectures(self):
        self.students(2)
        self.db.start_lecture("EN2090", "L1", T0)
        self.db.rename_module("EN2090", "EN2091")
        self.assertEqual(self.db.get_student(1000)["modules"], ["EN2091"])
        self.assertEqual(self.db.list_lectures()[0]["module_code"], "EN2091")
        self.db.upsert_module("MA1010")
        with self.assertRaises(D.DbError):
            self.db.rename_module("EN2091", "MA1010")
        with self.assertRaises(D.DbError):
            self.db.rename_module("NOPE", "X")

    def test_delete(self):
        self.students(2)
        self.db.start_lecture("EN2090", "L1", T0)
        with self.assertRaises(D.DbError) as cm:
            self.db.delete_module("EN2090")
        self.assertIn("1 lecture", str(cm.exception))
        self.db.delete_module("EN2090", force=True)
        self.assertIsNone(self.db.get_module("EN2090"))
        self.assertEqual(self.db.get_student(1000)["modules"], [])
        self.assertEqual(self.db.counts()["lectures"], 0)
        self.assertEqual(self.db.counts()["students"], 2, "students stay")


class Taps(Base):
    def test_import_is_idempotent(self):
        rows = [(hms(5), 1000), (hms(9), 1001), (hms(12), 1000)]
        self.assertEqual(self.db.add_taps(rows, 7), 3)
        self.assertEqual(self.db.add_taps(rows, 7), 0, "reading the same file again adds nothing")
        self.assertEqual(self.db.add_taps(rows + [(hms(30), 1002)], 7), 1)
        self.assertEqual(self.db.tap_count(), 4)
        self.assertEqual(self.db.add_taps(rows, 8), 3, "the same taps from another device are different taps")
        self.assertEqual(self.db.last_sync()["rows_new"], 3)
        self.assertEqual(self.db.add_taps([], 7), 0)

    def test_unregistered_cards(self):
        self.students(2)
        self.db.add_taps([(hms(1), 1000), (hms(2), 777), (hms(3), 777), (hms(9), 888)], 0)
        u = self.db.unregistered_cards()
        self.assertEqual([c["card_id"] for c in u], [888, 777], "newest first")
        self.assertEqual(u[1]["taps"], 2)
        self.assertEqual(u[1]["first"], "2026-10-06 09:00:02")
        self.db.upsert_student(777, "Now Registered")
        self.assertEqual([c["card_id"] for c in self.db.unregistered_cards()], [888])

    def test_listing_joins_the_student(self):
        self.students(1)
        self.db.add_taps([(hms(1), 1000), (hms(2), 555)], 0)
        t = self.db.list_taps()
        self.assertEqual([(x["card_id"], x["name"]) for x in t], [(555, None), (1000, "Student 0")])
        self.assertEqual(len(self.db.list_taps(ts_from=hms(2))), 1)
        self.assertEqual(len(self.db.list_taps(ts_to=hms(2))), 1)
        self.assertEqual(len(self.db.list_taps(card_id=1000)), 1)
        rows = list(csv.reader(io.StringIO(self.db.taps_csv())))
        self.assertEqual(rows[0], ["date", "time", "card_id", "name", "student_no", "department"])
        self.assertEqual(rows[1][:3], ["2026-10-06", "09:00:01", "0000001000"])
        self.assertEqual(rows[2][3], "")


class Lectures(Base):
    def test_start_and_end(self):
        a = self.db.start_lecture("EN2090", "Lecture 1", T0)
        self.assertTrue(a["running"] and a["end"] == T0 + D.MAX_LECTURE_SECONDS, "an open lecture lasts at most six hours")
        b = self.db.start_lecture("EN2090", "Lecture 2", T0 + 7200)
        a = self.db.get_lecture(a["id"])
        self.assertEqual((a["end_ts"], a["end"]), (T0 + 7200, T0 + 7200), "starting the next ends the previous")
        self.assertEqual(self.db.current_lecture()["id"], b["id"])
        done = self.db.end_lecture(b["id"], T0 + 9000)
        self.assertEqual(done["end"], T0 + 9000)
        self.assertIsNone(self.db.current_lecture())

    def test_validation(self):
        for m, t in [("", "L"), ("M", ""), ("  ", "L")]:
            with self.assertRaises(D.DbError):
                self.db.start_lecture(m, t, T0)
        with self.assertRaises(D.DbError):
            self.db.create_lecture("M", "L", T0, T0)
        with self.assertRaises(D.DbError):
            self.db.create_lecture("M", "L", T0, T0 - 1)
        with self.assertRaises(D.DbError):
            self.db.update_lecture(999, title="x")
        with self.assertRaises(D.DbError):
            self.db.end_lecture(999)
        with self.assertRaises(D.DbError):
            self.db.delete_lecture(999)
        self.assertEqual(self.db.counts()["lectures"], 0)

    def test_titles_sent_to_the_device_are_device_safe(self):
        l = self.db.start_lecture("EN2090", "Part 1, \"Intro\" " + "x" * 40, T0)
        self.assertEqual(l["title"], D.clean_device_text("Part 1, \"Intro\" " + "x" * 40, 32))
        self.assertLessEqual(len(l["title"].encode()), 32)
        self.assertEqual(self.db.create_lecture("EN2090", "A long manual title, kept in full " + "y" * 20, T0, T0 + 10)["title"][:20], "A long manual title,")

    def test_confirmation_by_the_device(self):
        a = self.db.start_lecture("EN2090", "Lecture 1", T0)
        b = self.db.start_lecture("EN2090", "Lecture 2", T0 + 3600)
        got = self.db.confirm_lecture_start("EN2090", "Lecture 2", T0 + 3700)
        self.assertEqual(got["start_ts"], T0 + 3700)
        self.assertEqual(self.db.get_lecture(a["id"])["end_ts"], T0 + 3700, "the one before ends where this one really starts")
        self.assertIsNone(self.db.confirm_lecture_start("EN2090", "Lecture 2", T0 + 5000), "only once")
        self.assertIsNone(self.db.confirm_lecture_start("EN2090", "Never started", T0))
        self.assertEqual(self.db.get_lecture(b["id"])["start_ts"], T0 + 3700)

    def test_manual_edit(self):
        l = self.db.create_lecture("MA1010", "Past lecture", T0, T0 + 3600)
        self.assertEqual(l["end"], T0 + 3600)
        l = self.db.update_lecture(l["id"], title="Renamed", start_ts=T0 + 60, end_ts=T0 + 7200)
        self.assertEqual((l["title"], l["start_ts"], l["end"]), ("Renamed", T0 + 60, T0 + 7200))
        l = self.db.update_lecture(l["id"], end_ts=None)
        self.assertTrue(l["running"])
        l = self.db.update_lecture(l["id"], module="EN2090")
        self.assertEqual(l["module_code"], "EN2090")
        with self.assertRaises(D.DbError):
            self.db.update_lecture(l["id"], start_ts=T0 + 99999, end_ts=T0)
        self.db.delete_lecture(l["id"])
        self.assertEqual(self.db.list_lectures(), [])

    def test_windows_never_overlap(self):
        a = self.db.create_lecture("EN2090", "A", T0, T0 + 10 * 3600)
        self.db.create_lecture("EN2090", "B", T0 + 3600, T0 + 7200)
        self.assertEqual(self.db.get_lecture(a["id"])["end"], T0 + 3600, "a later start cuts an earlier lecture short")

    def test_listing_and_filters(self):
        self.db.create_lecture("EN2090", "A", D.parse_ts("2026-10-01 09:00"), D.parse_ts("2026-10-01 10:00"))
        self.db.create_lecture("MA1010", "B", D.parse_ts("2026-10-05 09:00"), D.parse_ts("2026-10-05 10:00"))
        self.db.create_lecture("EN2090", "C", D.parse_ts("2026-10-08 09:00"), D.parse_ts("2026-10-08 10:00"))
        self.assertEqual([l["title"] for l in self.db.list_lectures()], ["C", "B", "A"], "newest first")
        self.assertEqual([l["title"] for l in self.db.list_lectures(module="EN2090")], ["C", "A"])
        self.assertEqual([l["title"] for l in self.db.list_lectures(date_from="2026-10-05")], ["C", "B"])
        self.assertEqual([l["title"] for l in self.db.list_lectures(date_to="2026-10-05")], ["B", "A"], "the end date is inclusive")
        self.assertEqual([l["title"] for l in self.db.list_lectures(date_from="2026-10-02", date_to="2026-10-07")], ["B"])
        with self.assertRaises(D.DbError):
            self.db.list_lectures(date_from="nonsense")


class Attendance(Base):
    def lecture(self):
        self.students(5)                       # cards 1000..1004, all in EN2090
        self.db.upsert_student(2000, "Outsider", "", "Mechanical", ["MA1010"])
        return self.db.create_lecture("EN2090", "Lecture 1", T0, T0 + 3600)

    def test_present_absent_other_unregistered(self):
        l = self.lecture()
        self.db.add_taps([(hms(10), 1002), (hms(5), 1000), (hms(20), 2000), (hms(30), 7777), (hms(40), 1000)], 0)
        a = self.db.lecture_attendance(l["id"])
        self.assertEqual([p["card_id"] for p in a["present"]], [1000, 1002])
        self.assertEqual(a["present"][0]["time"], "09:00:05", "the first tap is the arrival")
        self.assertEqual([p["card_id"] for p in a["present_other"]], [2000], "registered but not enrolled in this module")
        self.assertEqual([p["card_id"] for p in a["absent"]], [1001, 1003, 1004])
        self.assertEqual([u["card_id"] for u in a["unregistered"]], [7777])
        c = a["counts"]
        self.assertEqual((c["enrolled"], c["present_enrolled"], c["present_other"], c["absent"], c["unregistered"]), (5, 2, 1, 3, 1))
        self.assertEqual(c["percent"], 40.0)

    def test_taps_outside_the_window_do_not_count(self):
        l = self.lecture()
        self.db.add_taps([(T0 - 1, 1000), (T0 + 3600, 1001), (T0, 1002), (T0 + 3599, 1003)], 0)
        a = self.db.lecture_attendance(l["id"])
        self.assertEqual([p["card_id"] for p in a["present"]], [1002, 1003], "start is included, end is not")

    def test_the_same_card_is_one_attendance(self):
        l = self.lecture()
        self.db.add_taps([(hms(i), 1000) for i in range(0, 600, 60)], 0)
        a = self.db.lecture_attendance(l["id"])
        self.assertEqual(len(a["present"]), 1)
        self.assertEqual(a["counts"]["taps"], 1)

    def test_percentage_with_no_enrolments(self):
        self.db.upsert_student(1, "Solo")
        l = self.db.create_lecture("NEW1", "L", T0, T0 + 100)
        self.db.add_taps([(hms(1), 1)], 0)
        a = self.db.lecture_attendance(l["id"])
        self.assertEqual(a["counts"]["percent"], 0.0)
        self.assertEqual(len(a["present_other"]), 1)

    def test_unknown_lecture(self):
        with self.assertRaises(D.DbError):
            self.db.lecture_attendance(404)

    def test_card_registered_afterwards_is_matched_retroactively(self):
        l = self.lecture()
        self.db.add_taps([(hms(5), 7777)], 0)
        self.assertEqual(len(self.db.lecture_attendance(l["id"])["unregistered"]), 1)
        self.db.upsert_student(7777, "Late Registration", modules=["EN2090"])
        a = self.db.lecture_attendance(l["id"])
        self.assertEqual([p["name"] for p in a["present"]], ["Late Registration"])
        self.assertEqual(a["unregistered"], [])

    def test_unassigned_taps_and_turning_them_into_a_lecture(self):
        self.students(3)
        self.db.create_lecture("EN2090", "Known", T0, T0 + 1800)
        d2 = D.parse_ts("2026-10-07 14:00:00")
        self.db.add_taps([(T0 + 10, 1000), (d2, 1000), (d2 + 30, 1001), (d2 + 90, 1002), (d2 + 90, 5555)], 0)
        days = self.db.unassigned_days()
        self.assertEqual(len(days), 1)
        self.assertEqual(days[0]["date"], "2026-10-07")
        self.assertEqual((days[0]["cards"], days[0]["taps"], days[0]["first"], days[0]["last"]), (4, 4, "14:00:00", "14:01:30"))
        l = self.db.lecture_from_unassigned("2026-10-07", "EN2090", "Catch-up")
        self.assertEqual((l["start_ts"], l["end"]), (d2, d2 + 91))
        self.assertEqual(self.db.unassigned_days(), [])
        self.assertEqual(self.db.lecture_attendance(l["id"])["counts"]["present_enrolled"], 3)
        with self.assertRaises(D.DbError):
            self.db.lecture_from_unassigned("2026-10-07", "EN2090", "Again")

    def test_a_running_lecture_stops_matching_after_six_hours(self):
        l = self.db.start_lecture("EN2090", "Open", T0)
        self.students(1)
        self.db.add_taps([(T0 + 3600, 1000), (T0 + D.MAX_LECTURE_SECONDS + 5, 1000)], 0)
        self.assertEqual(self.db.lecture_attendance(l["id"])["counts"]["taps"], 1)
        self.assertEqual(len(self.db.unassigned_days()), 1)


class Reports(Base):
    def setUp(self):
        super().setUp()
        self.students(4)                           # 1000..1003 in EN2090
        self.db.upsert_module("MA1010", "Maths")
        self.db.set_enrollment("MA1010", [1000], "add")
        self.l1 = self.db.create_lecture("EN2090", "L1", D.parse_ts("2026-10-01 09:00"), D.parse_ts("2026-10-01 10:00"))
        self.l2 = self.db.create_lecture("EN2090", "L2", D.parse_ts("2026-10-08 09:00"), D.parse_ts("2026-10-08 10:00"))
        self.l3 = self.db.create_lecture("MA1010", "M1", D.parse_ts("2026-10-02 09:00"), D.parse_ts("2026-10-02 10:00"))
        t = lambda d, s: D.parse_ts(d + " 09:00:00") + s
        self.db.add_taps([(t("2026-10-01", 5), 1000), (t("2026-10-01", 9), 1001), (t("2026-10-08", 3), 1000),
                          (t("2026-10-08", 4), 1002), (t("2026-10-02", 8), 1000), (t("2026-10-02", 9), 1003)], 0)

    def test_module_matrix(self):
        r = self.db.module_report("EN2090")
        self.assertEqual([l["title"] for l in r["lectures"]], ["L1", "L2"], "oldest first")
        rows = {s["card_id"]: s for s in r["students"]}
        self.assertEqual(rows[1000]["flags"], [1, 1])
        self.assertEqual(rows[1001]["flags"], [1, 0])
        self.assertEqual(rows[1002]["flags"], [0, 1])
        self.assertEqual(rows[1003]["flags"], [0, 0], "1003 tapped at a MA1010 lecture, which is not this module")
        self.assertEqual((rows[1000]["count"], rows[1000]["percent"]), (2, 100.0))
        self.assertEqual(rows[1001]["percent"], 50.0)
        self.assertEqual(r["summary"], {"lectures": 2, "students": 4, "average": 50.0, "below_75": 3})
        self.assertEqual(r["lectures"][0]["present"], 2)

    def test_date_range_and_empty(self):
        r = self.db.module_report("EN2090", date_from="2026-10-05")
        self.assertEqual([l["title"] for l in r["lectures"]], ["L2"])
        r = self.db.module_report("EN2090", date_from="2030-01-01")
        self.assertEqual((r["lectures"], r["summary"]["average"]), ([], 0.0))
        self.assertTrue(all(s["percent"] == 0.0 for s in r["students"]))
        with self.assertRaises(D.DbError):
            self.db.module_report("NOPE")

    def test_student_report(self):
        r = self.db.student_report(1000)
        by = {m["code"]: m for m in r["modules"]}
        self.assertEqual((by["EN2090"]["attended"], by["EN2090"]["total"], by["EN2090"]["percent"]), (2, 2, 100.0))
        self.assertEqual((by["MA1010"]["attended"], by["MA1010"]["total"]), (1, 1))
        self.assertEqual(r["taps"], 3)
        r = self.db.student_report(1001)
        self.assertEqual([i["attended"] for i in r["modules"][0]["lectures"]], [True, False])
        self.assertEqual(r["modules"][0]["lectures"][0]["time"], "09:00:09")
        with self.assertRaises(D.DbError):
            self.db.student_report(424242)

    def test_csv_exports(self):
        att = self.db.lecture_attendance(self.l1["id"])
        rows = list(csv.reader(io.StringIO(D.Database.lecture_csv(att))))
        self.assertEqual(rows[0], ["Module", "EN2090"])
        self.assertEqual(rows[1], ["Lecture", "L1"])
        self.assertEqual(rows[4], ["Present", "2 of 4 (50.0%)"])
        body = {r[0]: r for r in rows[7:]}
        self.assertEqual(body["0000001000"][4:], ["Present", "09:00:05"])
        self.assertEqual(body["0000001002"][4:], ["Absent", ""])
        rows = list(csv.reader(io.StringIO(D.Database.module_csv(self.db.module_report("EN2090")))))
        self.assertEqual(rows[0][-2:], ["PRESENT", "PERCENT"])
        self.assertEqual(rows[1][4:], ["P", "P", "2/2", "100.0"])
        rows = list(csv.reader(io.StringIO(D.Database.student_csv(self.db.student_report(1001)))))
        self.assertIn(["EN2090", "2026-10-08", "L2", "No", ""], rows)


class Storage(unittest.TestCase):
    def test_data_survives_closing_and_reopening(self):
        d = tempfile.mkdtemp(prefix="att-db-")
        path = os.path.join(d, "a.db")
        db = D.Database(path)
        db.upsert_student(5, "Kept", modules=["EN2090"])
        db.add_taps([(T0, 5)], 1)
        db.close()
        db = D.Database(path)
        self.assertEqual(db.get_student(5)["modules"], ["EN2090"])
        self.assertEqual(db.tap_count(), 1)
        self.assertEqual(db.add_taps([(T0, 5)], 1), 0)
        db.close()

    def test_backup_is_a_usable_database(self):
        db = D.Database(":memory:")
        db.upsert_student(5, "Backed up")
        data = db.backup_bytes()
        self.assertTrue(data.startswith(b"SQLite format 3"))
        d = tempfile.mkdtemp(prefix="att-bk-")
        p = os.path.join(d, "restored.db")
        with open(p, "wb") as f:
            f.write(data)
        again = D.Database(p)
        self.assertEqual(again.get_student(5)["name"], "Backed up")
        again.close()
        db.close()

    def test_a_newer_database_is_refused(self):
        d = tempfile.mkdtemp(prefix="att-new-")
        p = os.path.join(d, "n.db")
        con = sqlite3.connect(p)
        con.execute("PRAGMA user_version = 99")
        con.close()
        with self.assertRaises(D.DbError):
            D.Database(p)

    def test_failures_roll_back(self):
        db = D.Database(":memory:")
        db.upsert_student(5, "Original", modules=["A"])
        with self.assertRaises(D.DbError):
            db.set_enrollment("A", [5, 999], "set")           # 999 is not a student: nothing may change
        self.assertEqual(db.get_student(5)["modules"], ["A"])
        db.close()

    def test_foreign_keys_are_enforced(self):
        db = D.Database(":memory:")
        with self.assertRaises(sqlite3.IntegrityError):
            db.con.execute("INSERT INTO enrollments(card_id,module_code) VALUES(1,'X')")
        with self.assertRaises(sqlite3.IntegrityError):
            db.con.execute("INSERT INTO students(card_id,name,created_at,updated_at) VALUES(0,'x','','')")
        db.close()

    def test_two_threads_can_write_at_once(self):
        db = D.Database(":memory:")
        errors = []

        def work(base):
            try:
                for i in range(200):
                    db.upsert_student(base + i, "T%d" % (base + i))
                    db.add_taps([(T0 + i, base + i)], base)
            except Exception as e:      # pragma: no cover
                errors.append(e)

        ts = [threading.Thread(target=work, args=(b,)) for b in (1000, 5000, 9000)]
        for t in ts:
            t.start()
        for t in ts:
            t.join()
        self.assertEqual(errors, [])
        self.assertEqual(db.counts()["students"], 600)
        self.assertEqual(db.tap_count(), 600)
        db.close()

    def test_a_large_log_is_quick_to_report_on(self):
        import time
        db = D.Database(":memory:")
        for i in range(300):
            db.upsert_student(1000 + i, "S%03d" % i, modules=["EN2090"])
        for lec in range(40):
            st = T0 + lec * 86400
            db.create_lecture("EN2090", "L%02d" % lec, st, st + 3600)
            db.add_taps([(st + 5 + (i % 50), 1000 + i) for i in range(0, 300, 2)], 0)
        t0 = time.time()
        r = db.module_report("EN2090")
        db.list_lectures()
        self.assertLess(time.time() - t0, 3.0)
        self.assertEqual(r["summary"]["lectures"], 40)
        self.assertEqual(r["summary"]["average"], 50.0)
        db.close()


if __name__ == "__main__":
    unittest.main()

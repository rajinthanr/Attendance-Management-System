"""
Do the device and the app understand each other?

Both directions, with the real firmware code (Firmware/App/Src, built for the
host) rather than a copy of its rules:

  * what the app writes (SETTINGS.CSV) is what the firmware's parser reads;
  * what the firmware writes (ATTEND.CSV, STATUS.TXT, SETTINGS.CSV) is what the
    app's parsers read, and the app can sync from a folder holding exactly those.

Needs a C compiler: `cc` on this machine, or the WSL "Ubuntu" distro on Windows.
Skipped when there is neither.
"""
import os
import re
import shutil
import atexit
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FW = os.path.join(os.path.dirname(ROOT), "Firmware")
sys.path.insert(0, ROOT)

import attendance_app as app  # noqa: E402
import attendance_db as D  # noqa: E402

_CACHE = {}


def _wsl(p):
    p = os.path.abspath(p)
    return "/mnt/" + p[0].lower() + p[2:].replace("\\", "/")


def _build():
    """A dict of tool name -> function(args) -> stdout, or None if nothing can be built."""
    if "tools" in _CACHE:
        return _CACHE["tools"]
    tools = None
    src = [os.path.join(FW, "App", "Src", f) for f in sorted(os.listdir(os.path.join(FW, "App", "Src"))) if f.endswith(".c")]
    inc = os.path.join(FW, "App", "Inc")
    plat = os.path.join(FW, "Tests", "host_platform.c")
    work = tempfile.mkdtemp(prefix="att-fw-")
    names = {"parse_settings": "parse_settings.c", "vol_image": "vol_image.c"}
    try:
        if shutil.which("cc"):
            tools = {}
            for n, c in names.items():
                out = os.path.join(work, n)
                r = subprocess.run(["cc", "-std=c11", "-O1", "-I" + inc, "-o", out] + src + [plat, os.path.join(FW, "Tests", c)],
                                   capture_output=True, text=True)
                if r.returncode != 0:
                    raise RuntimeError(r.stderr)
                tools[n] = (lambda o: lambda args: subprocess.run([o] + args, capture_output=True, text=True).stdout)(out)
        elif os.name == "nt" and shutil.which("wsl"):
            tools = {}
            for n, c in names.items():
                out = "/tmp/att_compat_" + n
                cmd = ["wsl", "-d", "Ubuntu", "-e", "cc", "-std=c11", "-O1", "-I" + _wsl(inc), "-o", out] + \
                      [_wsl(s) for s in src] + [_wsl(plat), _wsl(os.path.join(FW, "Tests", c))]
                r = subprocess.run(cmd, capture_output=True, text=True)
                if r.returncode != 0:
                    raise RuntimeError(r.stderr)
                tools[n] = (lambda o: lambda args: subprocess.run(["wsl", "-d", "Ubuntu", "-e", o] + args, capture_output=True,
                                                                  text=True, encoding="utf-8").stdout)(out)
    except (RuntimeError, OSError) as e:
        print("firmware compile failed: %s" % str(e)[:300], file=sys.stderr)
        tools = None
    # The cc tools live in `work`, so it can only go once the tests are done.
    atexit.register(shutil.rmtree, work, True)
    _CACHE["tools"] = tools
    _CACHE["wsl"] = tools is not None and not shutil.which("cc")
    return tools


def _path(p):
    return _wsl(p) if _CACHE.get("wsl") else p


def _parse_output(text):
    out, cur = {}, None
    for line in text.splitlines():
        if line.startswith("FILE "):
            cur = out.setdefault(os.path.basename(line[5:].replace("\\", "/")), {})
        elif cur is None:
            continue
        elif line.startswith("status="):
            m = re.match(r"status=(-?\d+) time=(\d) device=(\S+) new=(\d) bad=(\d)", line)
            cur.update(status=int(m.group(1)), time=m.group(2) == "1", device=None if m.group(3) == "-" else int(m.group(3)),
                       new=m.group(4) == "1", bad=m.group(5) == "1")
        elif line.startswith("time_value="):
            cur["time_value"] = line[len("time_value="):]
        elif line.startswith("module="):
            cur["module"] = line[len("module="):]
        elif line.startswith("lecture="):
            cur["lecture"] = line[len("lecture="):]
        elif line.startswith("cards="):
            v = line[len("cards="):]
            cur["cards"] = None if v == "-" else int(v)
        elif line.startswith("cards_crc="):
            cur["cards_crc"] = int(line[len("cards_crc="):], 16)
        elif line.startswith("clear="):
            cur["clear"] = line[len("clear="):] == "1"
    return out


@unittest.skipIf(_build() is None, "no C compiler for the firmware")
class AppToFirmware(unittest.TestCase):
    """Settings files the app can write, read by the firmware's own parser."""

    CASES = {
        "plain": dict(module="EN2090", lecture="Circuits Lecture 4"),
        "everything": dict(now=D.parse_ts("2030-05-06 07:08:09"), module="EN2090", lecture="Lecture 4", new_session=True, device_id=12345),
        "blank_names": dict(module="", lecture=""),
        "commas_quotes": dict(module="EN,2090", lecture='Part 1, "Intro"'),
        "unicode": dict(module="සිංහල", lecture="Café ☕ talk 😀"),
        "long": dict(module="m" * 40, lecture="l" * 50),
        "longest_valid": dict(module="m" * 24, lecture="l" * 32),
        "utf8_edge": dict(module="a" * 23 + "é", lecture="b" * 31 + "é"),
        "device_only": dict(device_id=4294967294),
        "time_only": dict(now=D.parse_ts("2099-12-31 23:59:59")),
        "nothing": dict(),
        "newsession_only": dict(new_session=True),
        "cards": dict(cards=[1000, 7, 4000000000, 7, 424242]),
        "no_cards": dict(cards=[]),
        "cards_and_names": dict(module="EN2090", lecture="Lecture 4", device_id=12345, cards=list(range(100000, 101000))),
        "cards_largest": dict(cards=[1, 0xFFFFFEFF]),
        "new_lecture_and_clear": dict(now=D.parse_ts("2030-05-06 07:08:09"), module="EN2090", lecture="Lecture 5",
                                      new_session=True, cards=[1000, 1007], clear_log=True),
    }

    def test_every_case_reads_back_as_intended(self):
        tools = _build()
        d = tempfile.mkdtemp(prefix="att-c1-")
        try:
            files = []
            for name, kw in self.CASES.items():
                p = os.path.join(d, name + ".csv")
                with open(p, "w", encoding="utf-8", newline="") as f:
                    f.write(D.build_settings(**kw))
                files.append(p)
            got = _parse_output(tools["parse_settings"]([_path(p) for p in files]))
            for name, kw in self.CASES.items():
                g = got.get(name + ".csv")
                self.assertIsNotNone(g, name)
                self.assertEqual(g["status"], 0, name)
                self.assertFalse(g["bad"], name + ": no value was malformed")
                self.assertEqual(g["time"], "now" in kw, name)
                if "now" in kw:
                    self.assertEqual(g["time_value"], D.fmt_ts(kw["now"]), name)
                self.assertEqual(g["new"], bool(kw.get("new_session")), name)
                self.assertEqual(g["clear"], bool(kw.get("clear_log")), name + ": the log is cleared only when asked")
                self.assertEqual(g["device"], kw.get("device_id") or None, name)
                if kw.get("cards") is None:
                    self.assertIsNone(g["cards"], name + ": no #CARDS line, the device keeps its list")
                else:
                    self.assertEqual(g["cards"], len(set(kw["cards"])), name)
                    self.assertEqual(g["cards_crc"], D.cards_crc(kw["cards"]) if kw["cards"] else 0xFFFFFFFF ^ 0xFFFFFFFF, name)
                if "module" in kw:
                    self.assertEqual(g["module"], D.clean_device_text(kw["module"], D.MODULE_BYTES), name)
                    self.assertLessEqual(len(g["module"].encode()), 24)
                else:
                    self.assertEqual(g["module"], "", name)
                if "lecture" in kw:
                    self.assertEqual(g["lecture"], D.clean_device_text(kw["lecture"], D.LECTURE_BYTES), name)
                    self.assertLessEqual(len(g["lecture"].encode()), 32)
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_a_file_the_app_made_for_the_device_to_show_back(self):
        """What the app writes, the device later shows in STATUS.TXT; the app must read that the same."""
        s = D.parse_status("ATTENDANCE LOGGER\r\nLecture      : EN2090 / Circuits Lecture 4 (since 2030-05-06 07:08:09)\r\n")
        self.assertEqual((s["module"], s["lecture"]), (D.clean_device_text("EN2090", 24), D.clean_device_text("Circuits Lecture 4", 32)))


@unittest.skipIf(_build() is None, "no C compiler for the firmware")
class FirmwareToApp(unittest.TestCase):
    """The files the real firmware generates, read by the app and synced into a database."""

    def files(self, big=False, lectures=False):
        d = tempfile.mkdtemp(prefix="att-c2-")
        self.addCleanup(shutil.rmtree, d, True)
        args = ["files", _path(d)] + (["big"] if big else []) + (["lectures"] if lectures else [])
        out = _build()["vol_image"](args)
        self.assertIn("wrote ATTEND.CSV", out)
        return d

    def test_the_apps_parsers_read_what_the_firmware_writes(self):
        d = self.files()
        with open(os.path.join(d, "STATUS.TXT"), encoding="utf-8") as f:
            st = D.parse_status(f.read())
        self.assertTrue(st["ok"])
        self.assertEqual(st["device_id"], 12648430)
        self.assertEqual(st["records"], 60)
        self.assertEqual((st["module"], st["lecture"]), ("EN2090", "Lecture 1"))
        self.assertIsNotNone(st["since"], "the firmware reports when the lecture started")
        self.assertEqual(st["last_card"], 1028)
        self.assertFalse(st["pending"])
        self.assertEqual(st["error"], "")
        known = [1000, 1007, 1014, 1021, 1028, 5000, 777777]
        self.assertEqual(st["cards"], len(known), "the device reports how many cards it holds")
        self.assertEqual(st["cards_crc"], D.cards_crc(known), "and the CRC the app computes is the firmware's")
        with open(os.path.join(d, "SETTINGS.CSV"), encoding="utf-8", newline="") as f:
            shown = f.read()
        self.assertIn("\r\n#CARDS,0007\r\n0000001000\r\n0000001007\r\n", shown)
        back = _parse_output(_build()["parse_settings"]([_path(os.path.join(d, "SETTINGS.CSV"))]))["SETTINGS.CSV"]
        self.assertEqual((back["status"], back["cards"], back["cards_crc"]), (0, 7, D.cards_crc(known)),
                         "the file the device shows parses back to the same list")
        with open(os.path.join(d, "ATTEND.CSV"), encoding="utf-8", newline="") as f:
            raw = f.read()
        rows, skipped = D.parse_attend(raw)
        self.assertEqual((len(rows), skipped), (60, 0))
        self.assertEqual(rows[0], (D.parse_ts("2026-10-06 09:30:00"), 1000))
        self.assertEqual(rows[-1][1], 1028)
        self.assertEqual(st["last_tap"], rows[-1][0], "STATUS.TXT agrees with the last row")
        self.assertEqual(len(raw) % 32, 0)

    def test_the_lecture_list_reads_back(self):
        d = self.files()
        with open(os.path.join(d, "LECTURES.CSV"), encoding="utf-8", newline="") as f:
            raw = f.read()
        self.assertEqual(len(raw), 2 * 128, "the header and one 128-byte row")
        self.assertTrue(raw.startswith("DATE,TIME,MODULE,LECTURE ") and raw.endswith("\r\n"))
        rows, skipped = D.parse_lectures(raw)
        self.assertEqual(skipped, 0)
        self.assertEqual(rows, [(D.parse_ts("2026-10-06 09:48:30"), "EN2090", "Lecture 1")])
        with open(os.path.join(d, "STATUS.TXT"), encoding="utf-8") as f:
            self.assertEqual(D.parse_status(f.read())["since"], rows[0][0], "the same start as STATUS.TXT")

    def test_lectures_started_on_the_device_each_get_their_taps(self):
        """A second lecture, started from the button and named by the firmware, splits the taps."""
        d = self.files(lectures=True)
        with open(os.path.join(d, "LECTURES.CSV"), encoding="utf-8", newline="") as f:
            rows, skipped = D.parse_lectures(f.read())
        self.assertEqual(skipped, 0)
        self.assertEqual([(m, t) for _, m, t in rows], [("EN2090", "Lecture 1"), ("EN2090", "Lecture 2")],
                         "the firmware numbers the name on")
        data = tempfile.mkdtemp(prefix="att-c2l-")
        self.addCleanup(shutil.rmtree, data, True)
        db = D.Database(os.path.join(data, "a.db"))
        self.addCleanup(db.close)
        a = app.App(db, d, data)
        s = a.refresh()
        self.assertEqual(s["sync"]["new"], 60)
        self.assertEqual(s["sync"]["lectures"], 2)
        lecs = {l["title"]: l for l in db.list_lectures()}
        self.assertEqual(set(lecs), {"Lecture 1", "Lecture 2"})
        self.assertEqual(lecs["Lecture 1"]["end_ts"], rows[1][0], "lecture 1 ends where lecture 2 starts")
        self.assertEqual(db.current_lecture()["title"], "Lecture 2", "the newest is the one running")
        # Records 30..44 fall in lecture 1 and 45..59 in lecture 2: 15 taps each, cards 1000..1028.
        for title in ("Lecture 1", "Lecture 2"):
            self.assertEqual(lecs[title]["counts"]["taps"], 5, title + ": 5 distinct cards")
        self.assertTrue(a.import_everything(), "everything read: the device may be cleared")

    def test_a_full_log_is_read_whole(self):
        d = self.files(big=True)
        with open(os.path.join(d, "ATTEND.CSV"), encoding="utf-8", newline="") as f:
            rows, skipped = D.parse_attend(f.read())
        self.assertEqual((len(rows), skipped), (13900, 0))
        self.assertEqual(rows[0][1], 1000)
        with open(os.path.join(d, "STATUS.TXT"), encoding="utf-8") as f:
            st = D.parse_status(f.read())
        self.assertEqual(st["records"], 13900)

    def test_the_app_syncs_from_a_folder_the_firmware_made(self):
        d = self.files()
        data = tempfile.mkdtemp(prefix="att-c2d-")
        self.addCleanup(shutil.rmtree, data, True)
        db = D.Database(os.path.join(data, "a.db"))
        self.addCleanup(db.close)
        a = app.App(db, d, data)
        s = a.refresh()
        self.assertTrue(s["connected"])
        self.assertEqual(s["sync"]["new"], 60)
        self.assertEqual(s["counts"]["taps"], 60)
        self.assertEqual(s["device"]["device_id"], 12648430)
        self.assertEqual(s["device"]["lecture"], "Lecture 1")
        lec = db.current_lecture()
        self.assertEqual((lec["module_code"], lec["title"]), ("EN2090", "Lecture 1"), "the lecture the device runs is adopted")
        self.assertEqual(lec["start_text"], s["device"]["since"], "with the device's own start time")
        self.assertEqual(a.refresh()["sync"]["seq"], 1, "and nothing is re-read until it changes")
        self.assertEqual(s["cards"], {"device": 7, "database": 0, "in_sync": False, "too_many": False},
                         "the device holds cards the (empty) database does not")
        # Cards 1000..1028 tapped in the second half belong to the lecture; the first 30 taps were before it.
        db.upsert_student(1000, "Card One", modules=["EN2090"])
        att = db.lecture_attendance(lec["id"])
        self.assertEqual([p["card_id"] for p in att["present"]], [1000])
        self.assertEqual(att["counts"]["taps"], 5, "5 distinct cards tapped after the lecture started")
        self.assertEqual(len(db.unassigned_days()), 1, "the first 30 taps happened before it")

    def test_settings_written_by_the_app_survive_the_round_trip_to_the_device_files(self):
        """The app starts a lecture; the firmware's parser reads the very file the app wrote to the drive."""
        d = self.files()
        data = tempfile.mkdtemp(prefix="att-c2e-")
        self.addCleanup(shutil.rmtree, data, True)
        db = D.Database(os.path.join(data, "a.db"))
        self.addCleanup(db.close)
        a = app.App(db, d, data)
        a.refresh()
        a.write_settings(D.build_settings(now=D.now_ts(), module="MA1010", lecture="Calculus Lecture 3", new_session=True,
                                          device_id=a.status["device_id"]))
        got = _parse_output(_build()["parse_settings"]([_path(os.path.join(d, "SETTINGS.CSV"))]))["SETTINGS.CSV"]
        self.assertEqual((got["status"], got["module"], got["lecture"], got["new"], got["time"], got["device"]),
                         (0, "MA1010", "Calculus Lecture 3", True, True, 12648430))


if __name__ == "__main__":
    unittest.main()

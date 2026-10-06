"""
The card list, app to firmware: whatever the app sends, the firmware's own parser
reads the way the app meant, and lists the firmware would refuse are lists the
app refuses to build.
"""
import os
import shutil
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)

import attendance_db as D  # noqa: E402
import test_firmware_compat as C  # noqa: E402


@unittest.skipIf(C._build() is None, "no C compiler for the firmware")
class CardListCompat(unittest.TestCase):
    def parse(self, texts):
        d = tempfile.mkdtemp(prefix="att-cl-")
        self.addCleanup(shutil.rmtree, d, True)
        paths = []
        for name, txt in texts.items():
            p = os.path.join(d, name + ".csv")
            with open(p, "w", encoding="utf-8", newline="") as f:
                f.write(txt)
            paths.append(p)
        return C._parse_output(C._build()["parse_settings"]([C._path(p) for p in paths]))

    def test_lists_the_firmware_refuses(self):
        crlf = "\r\n"
        bad = {
            "descending": "#CARDS,2" + crlf + "20" + crlf + "10" + crlf,
            "repeated": "#CARDS,2" + crlf + "10" + crlf + "10" + crlf,
            "short": "#CARDS,3" + crlf + "10" + crlf + "20" + crlf,
            "zero": "#CARDS,1" + crlf + "0" + crlf,
            "marker_range": "#CARDS,1" + crlf + "4294967040" + crlf,
            "letters": "#CARDS,1" + crlf + "abc" + crlf,
        }
        got = self.parse(bad)
        for name in bad:
            self.assertNotEqual(got[name + ".csv"]["status"], 0, name + " must be refused by the device")

    def test_the_app_will_not_build_what_the_device_refuses(self):
        for cards in ([0], [0xFFFFFF00], list(range(1, 1002))):
            with self.assertRaises(D.DbError, msg=str(cards)[:30]):
                D.build_settings(cards=cards)
        # Unsorted input with repeats is put in order, so the device accepts it.
        got = self.parse({"messy": D.build_settings(cards=[20, 10, 10, 5])})
        self.assertEqual((got["messy.csv"]["status"], got["messy.csv"]["cards"]), (0, 3))
        self.assertEqual(got["messy.csv"]["cards_crc"], D.cards_crc([5, 10, 20]))

    def test_a_thousand_cards_and_a_lecture_in_one_file(self):
        cards = [1000 + 3 * i for i in range(D.DEVICE_CARDS_MAX)]
        text = D.build_settings(now=D.now_ts(), module="EN2090", lecture="Lecture 4", new_session=True,
                                device_id=12345, cards=cards)
        self.assertLessEqual(len(text.encode()), 28 * 512, "fits the device's USB window")
        got = self.parse({"full": text})["full.csv"]
        self.assertEqual((got["status"], got["cards"], got["device"], got["module"], got["new"]),
                         (0, 1000, 12345, "EN2090", True))
        self.assertEqual(got["cards_crc"], D.cards_crc(cards))


if __name__ == "__main__":
    unittest.main()

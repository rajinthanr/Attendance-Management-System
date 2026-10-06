# Attendance Logger: companion app

A small app for the computer that the attendance device is plugged into. The
device records only a card number and a time for each tap, and keeps a list of
registered card numbers. This app keeps everything else: who each card belongs to, their department and modules, and the
lectures that were held. It reads the taps off the device, matches them to
lectures, and makes attendance reports. You never have to open a CSV by hand,
though you can.

It needs **Python 3.8 or newer** and nothing else: no install, no internet.
It runs only on your own computer (it listens on 127.0.0.1) and uses your
normal browser for the screens.

## Start it

| System | How |
|---|---|
| Windows | double-click `start.bat` |
| macOS, Linux | `sh start.sh` |
| anywhere | `python attendance_app.py` |

A browser tab opens. Plug the device into the USB-C port: it appears as a drive
called **ATTENDANCE** and the green pill at the top says *Device connected*. It
can take a few seconds. Leave the black window open while you work; close it,
or press **Quit app** at the bottom of the page, to stop.

`python attendance_app.py --demo` shows the whole app with made-up data and no
device, which is a good way to learn it. Nothing in demo mode touches a real
drive or your database. Other options: `--port`, `--no-browser`,
`--device-dir` (use a folder as the device) and `--data-dir` (keep the database
elsewhere).

## The five tabs

1. **Lecture.** Choose or type the module and the lecture, leave *Set the device
   clock* ticked, press the start button, then eject the drive and unplug the
   cable. The device checks the file as the cable comes out: green light and two
   buzzes means the lecture started, red light and three buzzes means it was
   refused. Everyone who taps afterwards is recorded against that lecture, once.
   Starting a lecture also sends the device the list of student cards.
2. **Attendance.** Every lecture, with who was present, who was absent, who is
   not enrolled in the module, and cards nobody has registered. Search, print or
   save as PDF, or download as CSV. Taps that fall in no lecture are grouped by
   day, and a day can be turned into a lecture afterwards. The data stays on
   screen after you unplug.
3. **Students.** Name, student number, department and modules for each card. Add
   one, paste rows from Excel, or import a CSV. Cards the device recorded that
   belong to nobody are listed at the top under *New cards*, with a **Register**
   button. A banner (*The device does not have your latest student cards*) shows
   when the device's list is out of date, with a **Send cards to device** button.
4. **Modules.** The modules you teach, and who is enrolled in each.
5. **Reports.** Each student against each lecture, with a percentage, for one
   module (optionally between two dates), or one student across their modules.
   Print it or download it as CSV or PDF.

## Registering a new card

The device's reader is off while it is plugged in, so there is no live
enrolment. Tap the new card on the device, then plug the device in: the app reads
the log, and the card appears under *New cards* on the Students tab. Press
**Register** and fill in the details, then press **Send cards to device** and
unplug the cable. Until the device has the card it shows red when the card is
tapped (green means registered); the tap is recorded either way.

## PDF

PDFs are made by asking your own Chrome or Edge to print the report page in the
background. If neither is installed, open the report in the browser and use
Print, then Save as PDF.

## Your data

The database is one SQLite file, `~/AttendanceLogger/attendance.db` (your home
folder), holding students, modules, enrolments, lectures and every tap. It makes a
dated copy in `~/AttendanceLogger/backups/` once a day and keeps the last 14.
**Back up the database** at the bottom of the page downloads a copy now. If you
only have a saved `ATTEND.CSV` and not the device, **Import an ATTEND.CSV…** adds
its taps. Reading the device twice never duplicates a tap.

## How it works, and what it will not do

* It looks for the device by checking each drive for `ATTEND.CSV`,
  `SETTINGS.CSV` and a `STATUS.TXT` that begins `ATTENDANCE LOGGER`, so another
  stick that happens to hold files with those names is never touched.
* It reads `ATTEND.CSV` and `STATUS.TXT`, and writes only `SETTINGS.CSV` (the
  clock, module, lecture, `#NEWSESSION` and the `#CARDS` list). It never writes `ATTEND.CSV`; the
  device could not accept that anyway.
* Changes reach the device when the cable is unplugged, because that is when the
  device reads the file.
* A lecture is the device's own idea: it stores the module and lecture names in
  its log, so taps are separated by lecture even if you never open this app. The
  app matches taps to its lectures by time, using the start time the device
  reports in `STATUS.TXT` (the device's clock, whatever this computer says). A
  lecture with no end time lasts until the next one starts, or six hours.
* The device holds only card numbers, never names. The app sends the full list
  (every student's card number, sorted) with every lecture start and clock set,
  and with **Send cards to device**. It compares the count and CRC on
  `STATUS.TXT` with its own list to decide whether the device is out of date. The
  device holds at most 1000 cards; the app refuses to send more (delete students
  who have left first).

## Files

| File | Purpose |
|---|---|
| `attendance_app.py` | the helper: finds the device, reads and writes its files, serves the page and its API |
| `attendance_db.py` | the SQLite database, the rules for matching taps to lectures, the `SETTINGS.CSV` writer (with the card list and its CRC), and the parsers for `ATTEND.CSV` and `STATUS.TXT` |
| `report_pages.py` | printable report pages and the PDF export |
| `web/` | the screens (`index.html`, `app.js`, `style.css`) |
| `tests/` | see below |

## Tests

From this folder:

```
python -m unittest discover -s tests    # database, server, device and card-list compatibility
node tests/ui_smoke.js                  # drives the real page in headless Chrome or Edge, on demo data
```

`test_firmware_compat.py` builds the firmware's own parser and generators for the
host (it needs a C compiler: `cc`, or the WSL Ubuntu distro on Windows, and is
skipped without one) and checks both directions: the app's `SETTINGS.CSV` is
read by the firmware, and the firmware's `ATTEND.CSV`, `STATUS.TXT` and
`SETTINGS.CSV` are read by the app. `test_card_list_compat.py` does the same for
the `#CARDS` list: what the app writes, what the firmware accepts or refuses, and
the CRC. Run them after changing `settings_file.c`, `csv.c`, `device_cfg.c`, the
`STATUS.TXT` layout in `usb_storage.c`, or `attendance_db.py`.

## Troubleshooting

* *"Waiting for the device" for a long time.* Check the drive shows up in File
  Explorer or Finder. If it does and the app still waits, the drive is probably
  not this device (see above).
* *The device refused the settings.* The device keeps its old settings; read
  `STATUS.TXT` on the drive for the reason.
* *A registered card shows red.* The device does not have it yet. Press **Send
  cards to device** and unplug the cable.
* *Nobody shows as present.* Check the device clock (the app warns when it is
  more than two minutes off) and that the lecture was started before the taps.
* *Only one copy runs.* Starting a second just opens the first in your browser.

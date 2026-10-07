# Attendance Logger: companion app

A small app for the computer that the attendance device is plugged into. The
device records only a card number and a time for each tap, and keeps a list of
registered card numbers. This app keeps everything else: who each card belongs to, their department and modules, and the
lectures that were held. It reads the taps off the device, matches them to
lectures, and makes attendance reports. You never have to open a CSV by hand,
though you can.

It needs **Python 3.8 or newer** and nothing else: no install, no internet.
It opens its own window on your computer. Nothing is served on the network,
and no browser is needed except for printing. The window uses Tk, which comes
with the python.org installers for Windows and macOS. On Linux, install the
`python3-tk` package.

## Start it

| System | How |
|---|---|
| Windows | double-click `start.bat` |
| macOS, Linux | `sh start.sh` |
| anywhere | `python attendance_gui.py` |

The **Attendance Logger** window opens. Along the top are the device status
and a **Read the device now (F5)** button. Under them, banners appear when
something needs doing. The five tabs fill the middle. Along the bottom are
**Import an ATTEND.CSV…**, **Back up the database…**, **Set the device clock**
and **Quit**, and below those a status line that reports what just happened.

Plug the device into the USB-C port: it appears as a drive called
**ATTENDANCE**, and the status at the top turns green and says *Device
connected*. That can take a few seconds. The window keeps working while it
looks for the device. To stop, close the window or press **Quit**. On Windows
a small black window opens behind it; leave that open, since closing it closes
the app.

`python attendance_gui.py --demo` shows the whole app with made-up data and no
device, which is a good way to learn it. Nothing in demo mode touches a real
drive or your database, and the made-up data is deleted when you quit. Other
options: `--device-dir` (use a folder as the device) and `--data-dir` (keep the
database elsewhere).

The window works from the keyboard too. Ctrl+1 to Ctrl+5 or Alt and the
underlined letter pick a tab, Ctrl+Tab moves to the next one, Ctrl+F jumps to
the search box, F5 reads the device, and Ctrl+Q quits. In lists, Enter or a
double-click opens the selected row, and clicking a column heading sorts by
it. In a dialog, Enter saves and Escape cancels. Every delete asks first.

## The five tabs

1. **Lecture.** Choose or type the module and the lecture, leave *Set the device
   clock* ticked and press the start button. The app first reads every tap on
   the device into its database and, once it has all of them, tells the device
   to delete its copy. Then it writes the lecture and ejects the drive. The device
   checks the file: green light and two buzzes means the lecture started, red
   light and three buzzes means it was refused. The cable can stay in: the device
   takes attendance on USB power. If the eject fails, eject the drive yourself or
   tap the button on the device once. Everyone who taps afterwards is recorded
   against that lecture, once. Starting a lecture also sends the device the list
   of student cards. Unplug and plug the cable back in to read the taps.
2. **Attendance.** Every lecture on the left. Pick one to see who was present,
   who was absent, who is not enrolled in the module, and cards nobody has
   registered. Search, print, or save it as a PDF or CSV. Taps that fall in no
   lecture are grouped by day, and a day can be turned into a lecture
   afterwards. *Every tap* lists the newest 500 taps. The data stays on screen
   after you unplug.
3. **Students.** Name, student number, department and modules for each card. Add
   one, paste rows from Excel, or import a CSV. Cards the device recorded that
   belong to nobody are listed at the top under *New cards*, with a **Register**
   button. A banner (*The device does not have your latest student cards*) shows
   when the device's list is out of date, with a **Send cards to device** button.
4. **Modules.** The modules you teach, and who is enrolled in each.
5. **Reports.** Each student against each lecture, with a percentage, for one
   module (optionally between two dates), or one student across their modules.
   Print it, or save it as a CSV or PDF.

## Registering a new card

The device's reader is off while it is plugged in, so there is no live
enrolment. Tap the new card on the device, then plug the device in: the app reads
the log, and the card appears under *New cards* on the Students tab. Press
**Register** and fill in the details, then press **Send cards to device** and
eject the drive (or tap the button on the device). Until the device has the card it shows red when the card is
tapped (green means registered); the tap is recorded either way.

## Printing and PDF

**Print…** writes the report as a page on this computer and opens it in your
normal browser, which shows its Print window straight away. Choose a printer
there, or *Save as PDF*.

**Save PDF…** asks your own Chrome or Edge to print the report in the
background, then asks where to save the file. That can take a few seconds. If
neither browser is installed, the app says so; use **Print…** and choose
*Save as PDF* instead.

**Save CSV…** asks where to save the file. The CSV opens in Excel.

## Your data

The database is one SQLite file, `~/AttendanceLogger/attendance.db` (your home
folder), holding students, modules, enrolments, lectures and every tap. It makes a
dated copy in `~/AttendanceLogger/backups/` once a day and keeps the last 14.
**Back up the database…** at the bottom of the window saves a copy now, wherever
you choose. If you only have a saved `ATTEND.CSV` and not the device,
**Import an ATTEND.CSV…** adds its taps. Reading the device twice never
duplicates a tap. Next to the database, `companion-gui.json` remembers the last
module and the folder you last saved to, and `companion.lock` stops a second
window from opening on the same database.

## How it works, and what it will not do

* It looks for the device by checking each drive for `ATTEND.CSV`,
  `SETTINGS.CSV` and a `STATUS.TXT` that begins `ATTENDANCE LOGGER`, so another
  stick that happens to hold files with those names is never touched.
* It reads `ATTEND.CSV` and `STATUS.TXT`, and writes only `SETTINGS.CSV` (the
  clock, module, lecture, `#NEWSESSION`, `#CLEARLOG` and the `#CARDS` list). It never writes `ATTEND.CSV`; the
  device could not accept that anyway.
* Changes reach the device when the drive is ejected, the device's button is
  tapped or the cable is unplugged, because that is when the device reads the
  file. Starting a lecture ejects the drive for you (Windows: the same lock,
  dismount and eject Explorer uses; macOS: `diskutil eject`; Linux: `udisksctl`
  unmount and power-off).
* `#CLEARLOG` (delete the device's records) is sent only with a lecture start,
  and only when every row of `ATTEND.CSV` was read into the database and the row
  count matched `STATUS.TXT`. Otherwise the device keeps its records.
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
| `attendance_gui.py` | the window (Tk): the five tabs, the dialogs, printing and saving files |
| `attendance_app.py` | finds the device, reads and writes its files, the actions and exports both versions share, and the browser version's server |
| `attendance_db.py` | the SQLite database, the rules for matching taps to lectures, the `SETTINGS.CSV` writer (with the card list and its CRC), and the parsers for `ATTEND.CSV` and `STATUS.TXT` |
| `report_pages.py` | printable report pages and the PDF export |
| `web/` | the browser version's screens (`index.html`, `app.js`, `style.css`) |
| `tests/` | see below |

## Tests

From this folder:

```
python -m unittest discover -s tests    # database, server, window, device and card-list compatibility
node tests/ui_smoke.js                  # drives the browser version in headless Chrome or Edge, on demo data
```

`test_gui.py` opens the real window on demo data, goes through every tab and
dialog, reads the device and sends it the cards, then closes it. It is skipped
when there is no display. `test_actions.py` covers the actions and exports
that the window and the browser version share.

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
  cards to device** and eject the drive.
* *Nobody shows as present.* Check the device clock (the app warns when it is
  more than two minutes off) and that the lecture was started before the taps.
* *Only one copy runs.* Starting a second window on the same database just
  says the first is already open. Look for it on the taskbar.
* *"This Python has no Tk".* On Linux, install `python3-tk`. Elsewhere, install
  Python from python.org. Or use the browser version (below).

## The browser version

The same app also runs in your browser: `python attendance_app.py`. It serves
the screens to this computer only (127.0.0.1) and opens a browser tab. It shares
the database and the device logic with the window, so use one or the other,
not both at once. Its options are `--demo`, `--port`, `--no-browser`,
`--device-dir` and `--data-dir`. Starting it a second time just opens the first
copy in your browser, and **Quit app** at the bottom of the page stops it.

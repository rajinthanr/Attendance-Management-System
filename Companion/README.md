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

The **Attendance Logger** window opens: an indigo header with the device status
on the right, then a row of device buttons: **Read device** (blue, also F5),
**Set device time** (teal) and **Clear device records** (red). Under them,
banners appear when something needs doing (with an **Eject now** or **Send cards
to device** button where that is the fix). Two tabs fill the middle, and along
the bottom are **Back up…**, **Quit** and a status line that reports what just
happened.

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

The window works from the keyboard too. Ctrl+1 and Ctrl+2 or Alt and the
underlined letter pick a tab, Ctrl+Tab moves to the next one, Ctrl+F jumps to
the search box, F5 reads the device, and Ctrl+Q quits. In lists, Enter or a
double-click opens the selected row, and clicking a column heading sorts by
it. In a dialog, Enter saves and Escape cancels. Every delete asks first.

## The two tabs

The window shows only what a lecturer needs: lectures, and who came to each by
index number and name. (Modules, departments, enrolment and reports are still in
the database and in the browser version, `python attendance_app.py`, but not in
this window.)

1. **Lectures.** At the top, type the lecture name (the next one is suggested:
   *Circuits Lecture 4* becomes *Circuits Lecture 5*) and press **Start lecture**.
   The app first reads every tap on the device into its database and, once it has
   all of them, tells the device to delete its copy. Then it sets the device
   clock, writes the lecture and ejects the drive. The device checks the file:
   green light and two buzzes means the lecture started, red light and three
   buzzes means it was refused. The cable can stay in. Lectures started on the
   device itself (hold its button 2 seconds) appear here too the next time it is
   plugged in.

   Below, every lecture is listed, the running one marked with a dot. Pick one to
   see who came: **index number, name and the time they tapped**, with the count
   in a green badge. Cards that tapped but belong to nobody are pointed out in an
   orange strip with a **Register…** button. **Rename…**, **End lecture**,
   **Delete** and **Save list (CSV)…** work on the lecture shown.
2. **Students.** Index number, name, card and last tap for each student. **Add
   student**, or **Import list…** (paste rows from Excel or choose a CSV: card
   number, name, index number). Cards the device recorded that belong to nobody
   are listed at the top in an orange box, with a **Register…** button. **Send
   cards to device** gives the device the list, so registered cards show green.

## The device buttons

* **Set device time** sets the device clock to this computer's and ejects the
  drive so the device takes it at once.
* **Clear device records** empties the device's memory. It asks first. Every tap
  and lecture is copied into the database before the device is told to erase
  them (`#CLEARLOG`); if anything could not be read, nothing is erased. Students
  and everything already in the database are kept.

## Registering a new card

The device's reader is off while it is plugged in, so there is no live
enrolment. Tap the new card on the device, then plug the device in: the app reads
the log, and the card appears under *New cards* on the Students tab. Press
**Register**, fill in the index number and name, then press **Send cards to device** and
eject the drive (or tap the button on the device). Until the device has the card it shows red when the card is
tapped (green means registered); the tap is recorded either way.

## Saving a lecture list

**Save list (CSV)…** on the Lectures tab saves the lecture shown as a CSV with
the columns Index No, Name, Card and Time, unregistered cards last. It opens in
Excel. Printing and PDF reports are in the browser version.

## Your data

The database is one SQLite file, `~/AttendanceLogger/attendance.db` (your home
folder), holding students, modules, enrolments, lectures and every tap. It makes a
dated copy in `~/AttendanceLogger/backups/` once a day and keeps the last 14.
**Back up the database…** at the bottom of the window saves a copy now, wherever
you choose. If you only have a saved `ATTEND.CSV` and not the device,
**Import an ATTEND.CSV…** adds its taps. Reading the device twice never
duplicates a tap. Next to the database, `companion-gui.json` remembers the
folder you last saved to, and `companion.lock` stops a second
window from opening on the same database.

## How it works, and what it will not do

* It looks for the device by checking each drive for `ATTEND.CSV`,
  `SETTINGS.CSV` and a `STATUS.TXT` that begins `ATTENDANCE LOGGER`, so another
  stick that happens to hold files with those names is never touched.
* It reads `ATTEND.CSV`, `LECTURES.CSV` and `STATUS.TXT`, and writes only `SETTINGS.CSV` (the
  clock, module, lecture, `#NEWSESSION`, `#CLEARLOG` and the `#CARDS` list). It never writes `ATTEND.CSV`; the
  device could not accept that anyway.
* Changes reach the device when the drive is ejected, the device's button is
  tapped or the cable is unplugged, because that is when the device reads the
  file. Starting a lecture ejects the drive for you (Windows: the same lock,
  dismount and eject Explorer uses; macOS: `diskutil eject`; Linux: `udisksctl`
  unmount and power-off).
* `#CLEARLOG` (delete the device's records) is sent only with a lecture start,
  and only when every row of `ATTEND.CSV` and `LECTURES.CSV` was read into the
  database and the row count matched `STATUS.TXT`. Otherwise the device keeps its
  records.
* A lecture is the device's own idea: it stores the module and lecture names in
  its log, so taps are separated by lecture even if you never open this app. The
  app matches taps to its lectures by time, using the start times the device
  lists in `LECTURES.CSV` (the device's clock, whatever this computer says). A
  lecture with no end time lasts until the next one starts, or six hours.
* The lecturer can also start the next lecture on the device itself, with no
  computer: a long press. The device keeps the module and counts the lecture
  name on ("Circuits Lecture 4" becomes "Circuits Lecture 5"). `LECTURES.CSV`
  lists every lecture start in the device's log, so each lecture started this way
  becomes a lecture of its own here, with its own taps, however many there were
  between two reads. A lecture started on the device before any lecture was ever
  named has no module; it is filed under the module `UNASSIGNED`, and editing the
  lecture moves it to the right one. A lecture you delete here is not read back
  from the device. Older firmware has no `LECTURES.CSV`; then only the newest
  lecture, from `STATUS.TXT`, is read.
* The device holds only card numbers, never names. The app sends the full list
  (every student's card number, sorted) with every lecture start and clock set,
  and with **Send cards to device**. It compares the count and CRC on
  `STATUS.TXT` with its own list to decide whether the device is out of date. The
  device holds at most 1000 cards; the app refuses to send more (delete students
  who have left first).

## Files

| File | Purpose |
|---|---|
| `attendance_gui.py` | the window (Tk): the Lectures and Students tabs, the device buttons, the dialogs and saving files |
| `attendance_app.py` | finds the device, reads and writes its files, the actions and exports both versions share, and the browser version's server |
| `attendance_db.py` | the SQLite database, the rules for matching taps to lectures, the `SETTINGS.CSV` writer (with the card list and its CRC), and the parsers for `ATTEND.CSV`, `LECTURES.CSV` and `STATUS.TXT` |
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
dialog, reads the device, sends it the cards, sets its clock and clears its
records, then closes it. It is skipped
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

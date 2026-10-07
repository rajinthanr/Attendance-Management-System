# Attendance Logger: companion app

A small app for the computer that the attendance device is plugged into. The
device records only a card number and a time for each tap; it keeps no list of
registered cards and records every card it reads. This app keeps everything
else: who each card belongs to (so it alone decides which cards are
registered), their department and modules, and the lectures that were held. It reads the taps off the device, matches them to
lectures, and makes attendance reports. You never have to open a CSV by hand,
though you can.

It needs **Python 3.8 or newer**. It opens its own window on your computer;
nothing is served on the network.

The window is built with **Qt** (PySide6), which needs a one-time install, from
this folder:

```sh
python -m venv .venv
.venv/bin/pip install -r requirements.txt          # Windows: .venv\Scripts\pip install -r requirements.txt
```

`start.sh` / `start.bat` use that `.venv` by themselves. Without PySide6 the app
does not start; it says how to install it.

## Start it

| System | How |
|---|---|
| Windows | double-click `start.bat` |
| macOS, Linux | `sh start.sh` |
| anywhere | `python attendance_qt.py` (with the `.venv` Python, or one that has PySide6); `python attendance_app.py` opens the window too |

The **Attendance Logger** window opens: an indigo header with the device status
on the right, then a row of device buttons: **Connect** (indigo, Ctrl+K),
**Read device** (blue, also F5), **Eject** (orange, Ctrl+E), **Set device time**
(teal) and **Clear device records** (red). Under them, banners appear when
something needs doing (with an **Eject now** button where that is the fix). The
two pages, **Lectures** and **Students**, are picked from the bar on the left.
The menus (File, Device, View, Help) hold the same actions with their
shortcuts, and the status bar at the bottom reports what just happened.

Plug the device into the USB-C port: it appears as a drive called
**ATTENDANCE**, and the status at the top turns green and says *Device
connected*. That can take a few seconds. While the drive is gone the status
says *Device not connected*, and **Eject** and the other device buttons are
greyed out. Next to the status the device's **battery** is drawn as a phone
shows it: a battery filled from the left by its charge, green from 50 %, orange
from 20 %, red below, with the percentage beside it. Hover over it for the cell
voltage. While the device is plugged in its cell is charging, so the figure
reads a little high. Older firmware does not report the battery, nor does a
device that has not measured it yet, and then no battery is shown. The window
keeps working while it looks for the device. To stop, close the window
(File, Quit). On Windows a small black window opens behind it; leave that
open, since closing it closes the app.

`python attendance_qt.py --demo` shows the whole app with made-up data and no
device, which is a good way to learn it. Nothing in demo mode touches a real
drive or your database, and the made-up data is deleted when you quit. Other
options: `--device-dir` (use a folder as the device) and `--data-dir` (keep the
database elsewhere).

The window works from the keyboard too. Ctrl+1 and Ctrl+2 pick a page, Ctrl+F
jumps to the search box, F5 reads the device, Ctrl+K connects, Ctrl+E ejects,
and Ctrl+Q quits. In lists, Enter or a
double-click opens the selected row, and clicking a column heading sorts by
it. In a dialog, Enter saves and Escape cancels. Every delete asks first.

## The two pages

The window shows only what a lecturer needs: lectures, and who came to each by
index number and name. (Modules, departments and enrolments are still kept in
the database, but this window does not show them.)

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
   **Delete**, **Save list (CSV)…** and **Save as PDF…** work on the lecture
   shown. **Clear all records…**, above the list, empties this computer's
   records (see *Your data*).
2. **Students.** Index number, name, card and last tap for each student. **Add
   student**, or **Import list…** (paste rows from Excel or choose a CSV: card
   number, name, index number). **Tap a card…** reads a card on the plugged-in
   device (see below). Cards the device recorded that belong to nobody are
   listed at the top in an orange box, with a **Register…** button.

## The device buttons

* **Eject** ejects the device's drive safely, as the file manager's Eject does.
  The device then applies any changes waiting in `SETTINGS.CSV` and takes
  attendance with the cable still in: the window says *Ejected. The device is
  now taking attendance with the cable in.* When the computer refuses (on
  Linux, most often because a file manager window or the file indexer is
  showing the drive) the window says why, for example *The drive is in use by
  another program: close any window showing it and try again*.
* **Connect** reads the device now when its drive is there. When it is not
  (after an eject the device leaves the computer's USB bus, and the computer
  cannot call it back by itself), a note says how to bring it back: plug the
  cable in, or, if it is already plugged in and taking attendance, **press the
  device's button twice quickly**; its drive comes back in a few seconds. The
  window looks for it every second for 30 seconds, reads it as soon as it
  appears and closes the note. **Cancel** stops waiting.
* **Set device time** sets the device clock to this computer's and ejects the
  drive so the device takes it at once.
* **Clear device records** empties the device's memory. It asks first. Every tap
  and lecture is copied into the database before the device is told to erase
  them (`#CLEARLOG`); if anything could not be read, nothing is erased. Students
  and everything already in the database are kept.
* **Erase device without saving…** (in the Device menu) is for a device whose
  records cannot be read, which *Clear device records* then refuses to erase.
  It tells the device to erase everything **without** copying it here first, so
  taps and lectures not yet read in are lost for good. You have to type `ERASE`
  to confirm. The device keeps its number and clock and numbers its lectures
  from 1 again.

## Registering a card

The device shows green for every new tap: it does not know who is registered;
this app does. While the device is plugged in, its reader stays on, and a tap
is **not** attendance: the device flashes green, buzzes, and tells the app the
card's number (in `LASTCARD.TXT`). So, with the device plugged in:

* **New student:** press **+ Add student** and then **Tap card on device…**, or
  just **Tap a card…** on the Students page with no student picked. Tap the
  card on the device; its number fills in. Add the index number and name and
  save.
* **A new card for a student** (lost card, wrong number): pick the student in
  the list and press **Tap a card…**, or open the student and press **Tap card
  on device…**. The student's taps and modules move to the new card.
* **Whose card is this?** **Tap a card…** with no student picked: a card that
  belongs to someone selects that student.

A card that already belongs to another student is refused, and the app keeps
waiting for another card. Cancel stops the waiting. This needs the updated
firmware; an older device says nothing over USB, and then the way in is to tap
the new card with the device unplugged, plug it in, and **Register** the card
from the *New cards* list (or pick it in the student dialog).

## Saving lists (CSV and PDF)

**Save list (CSV)…** on the Lectures page saves the lecture shown as a CSV with
the columns Index No, Name, Card and Time, unregistered cards last. It opens in
Excel. **Save as PDF…** next to it saves the same list as a PDF to print.

**File, Export** saves the whole database's lists: **All attendance records**
(every tap: date, time, card, index number and name) and the **Student list**,
each as CSV or PDF. The student list CSV has every column (card, name, index
number, department, modules), so it can be imported again.

The PDFs are A4 tables with the title, when they were made, the column
headings at the top of every page and page numbers. The app writes them itself,
so no browser or other program is needed. They use the PDF's built-in
Helvetica, which has the letters of Western European languages only: other
letters (Sinhala and Tamil names, for instance) print as `?`, so use the CSV
for those.

## Your data

The database is one SQLite file, `~/AttendanceLogger/attendance.db` (your home
folder), holding students, modules, enrolments, lectures and every tap. It makes a
dated copy in `~/AttendanceLogger/backups/` once a day and keeps the last 14.
**File, Back up the database…** (Ctrl+B) saves a copy now, wherever you
choose.

**Clear all records on this computer…** (File menu, or the button above the
lecture list) deletes every lecture and every tap from the database, for a
fresh start; the students, their cards, modules and departments are kept.
First it saves a copy of the database in `~/AttendanceLogger/backups/`, named
with the date and time and `before-clear` (the daily copies do not replace it),
and says where. You have to type `CLEAR` to confirm. Records still on the
device come back the next time it is read, lectures and all, unless you tick
**Also clear the device's records**: with the device plugged in, it is then
told to erase its records too, without copying them here first. If you only have a saved `ATTEND.CSV` and not the device,
**Import an ATTEND.CSV…** adds its taps. Reading the device twice never
duplicates a tap. Next to the database, `companion-gui.json` remembers the
folder you last saved to, and `companion.lock` stops a second
window from opening on the same database.

## How it works, and what it will not do

* It looks for the device by checking each drive for `ATTEND.CSV`,
  `SETTINGS.CSV` and a `STATUS.TXT` that begins `ATTENDANCE LOGGER`, so another
  stick that happens to hold files with those names is never touched.
* The device's drive holds `ATTEND.CSV` (every tap), `LECTURES.CSV` (every
  lecture start), `SETTINGS.CSV`, `STATUS.TXT`, `LASTCARD.TXT` (the card last
  tapped while plugged in) and a `LECTURES` folder with one CSV per lecture,
  named like `L001_2026-10-07_14-30.csv` (lecture number, start date and time;
  taps before the first lecture are in `L000_…`). That folder is for reading
  by hand; the app does not need it.
* It reads `ATTEND.CSV`, `LECTURES.CSV`, `STATUS.TXT` and `LASTCARD.TXT`, and
  writes only `SETTINGS.CSV` (the clock, module, lecture, `#NEWSESSION` and
  `#CLEARLOG`). It never writes `ATTEND.CSV`; the device could not accept that
  anyway. The device makes `STATUS.TXT` and `LASTCARD.TXT` afresh at every
  read, so the app reads those two past the computer's file cache (Linux:
  `O_DIRECT`; macOS: `F_NOCACHE`; Windows: `FILE_FLAG_NO_BUFFERING`), falling
  back to an ordinary read where that fails.
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
  from the device. Taps made before the device's first lecture (its `L000` file)
  appear as **Lecture 0**, also under `UNASSIGNED`; move them by editing it, or
  delete it and it stays deleted. Older firmware has no `LECTURES.CSV`; then only the newest
  lecture, from `STATUS.TXT`, is read.
* The device holds only card numbers and times, never names, and no list of
  registered cards: the app sends it none (an older app's `#CARDS` list is
  ignored by the new firmware). Which taps are registered students is worked
  out here, from the database.

## Files

| File | Purpose |
|---|---|
| `attendance_qt.py` | the window (Qt, PySide6): the Lectures and Students pages, the device buttons, the battery icon, menus, dialogs and saving files |
| `app_helpers.py` | what the window says and decides, with no GUI in it: suggested names, banners, device status, the battery level, waiting for a tapped card or for the device to come back, a lecture's CSV and PDF, the one-window lock and the worker thread |
| `requirements.txt` | PySide6, for the Qt window |
| `attendance_app.py` | finds the device, reads and writes its files (`STATUS.TXT` and `LASTCARD.TXT` past the file cache), ejects it, and the actions and exports the window calls; running it opens the window |
| `attendance_db.py` | the SQLite database, the rules for matching taps to lectures, the `SETTINGS.CSV` writer, and the parsers for `ATTEND.CSV`, `LECTURES.CSV`, `STATUS.TXT` and `LASTCARD.TXT` |
| `report_pages.py` | printable report pages, and the PDF writer for tables (standard library only) |

`python attendance_qt.py --demo` is the quickest way to check a change by hand: the demo device has every file a real one has,
with a `LASTCARD.TXT` that never sees a tap.

## Troubleshooting

* *"Device not connected" for a long time.* Check the drive shows up in File
  Explorer or Finder. After an eject it does not: press **Connect** and then
  the device's button twice quickly. If the drive shows and the app still
  waits, the drive is probably not this device (see above).
* *Eject says the drive is in use.* Close any file manager window showing the
  ATTENDANCE drive and press **Eject** again, or press the button on the device
  once.
* *The device refused the settings.* The device keeps its old settings; read
  `STATUS.TXT` on the drive for the reason.
* *A card shows red on the device.* Red now means the device refused the
  settings file or something went wrong, not "unregistered": every new card
  shows green. Read `STATUS.TXT`.
* *Tap a card… keeps waiting.* The device must be plugged in, with the updated
  firmware (the drive then holds `LASTCARD.TXT`), and its light must flash green
  when the card is tapped.
* *Nobody shows as present.* Check the device clock (the app warns when it is
  more than two minutes off) and that the lecture was started before the taps.
* *Only one copy runs.* Starting a second window on the same database just
  says the first is already open. Look for it on the taskbar.
* *"needs PySide6".* Install it as described at the top (the `.venv` lines),
  then start the app with `start.sh` or `start.bat`.

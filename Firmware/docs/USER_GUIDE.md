# Card Attendance System: user guide

Written for: the people who run a class with the device, not the firmware team.

The device logs a card tap with the date and time, and shows its log to a
computer as a small USB drive. It records only the card number and the time: it
does not know who owns a card, or which cards are registered. For the easiest
way to use it, run the companion app in the `Companion` folder (see its README):
it keeps the student list, registers cards, finds the device, starts lectures,
shows who was present and prints reports. This guide describes the device
itself, which also works with no app at all, by opening the files on the drive.

## Taking attendance

Tap a card on the reader. The device answers:

| You see | It means |
|---|---|
| green light and one short buzz | the tap is recorded |
| two short buzzes | this card is already recorded in this lecture (or was read twice in a row); not recorded again |
| green light and three quick buzzes | a new lecture has started (you held the button, see below) |
| red light blinking five times | the battery is low |
| red light blinking fast without stopping, no buzz, nothing else works | the device could not start (most often its clock crystal). Unplug the USB cable and disconnect the battery for a few seconds, then try again; if it keeps happening, the board needs looking at |
| red light and four buzzes | the device's memory is full and the tap was not recorded (or, at switch-on, its reader did not start) |

Every card is recorded, registered or not: the device keeps no list of students
and shows green for every new tap. Who the card belongs to is worked out later,
on the computer.

While the device is plugged into a computer as a USB drive, a tap is **not**
recorded. It gives the same green light and buzz, and the card's number is shown
to the computer so the companion app can register the card (see
[Registering a new card](#registering-a-new-card)).

On battery, after three minutes without a tap the device switches itself off.
Press the power button to wake it (green light and a short buzz). While its
cable is in (a computer or a charger) it stays on.

The power button also does three other things while the device is on:

| Press | Result |
|---|---|
| tap | shows the battery: two green blinks if it is fine, red blinking five times if it is low. While it is a USB drive on a computer, a tap first ends that and starts taking attendance (see below) |
| two quick taps, with the cable in after an eject | brings the drive back to the computer, so the app can read the device again |
| hold until it buzzes (2 seconds), then let go | **starts the next lecture** (green and three quick buzzes). See [Starting a lecture on the device](#starting-a-lecture-on-the-device) |
| keep holding for 5 seconds | switches the device off (red light and a long buzz) instead; no lecture is started. Any changes you made to `SETTINGS.CSV` are applied first |

## Taking attendance with the cable in

When the device is plugged into a computer it shows up as a USB drive. Its
reader stays on, but only for registering cards: taps are not recorded while the
drive is there. To start taking attendance without pulling the cable, either:

- **eject the drive** on the computer (Eject in Explorer, Finder or the file
  manager; the companion app does this for you when you start a lecture), or
- **tap the button** on the device once.

The device applies your `SETTINGS.CSV` (green and two buzzes, or red and three
if the file was refused), the drive disappears from the computer, and taps are
recorded again. The cable can stay in: the device runs and charges from it. To
see the drive again, press the button twice quickly (or unplug the cable and
plug it back in); the app's **Connect** button reminds you. While the drive is
back on the computer, taps are for registering cards, not attendance, so eject
it again before the class taps in.

On a phone charger or power bank there is no drive: the device waits about five
seconds for a computer, then takes attendance as usual. A card held to it in
those first seconds is read once the wait is over; one taken away sooner gets
no light at all, so just tap it again.

## Lectures and double taps

A **lecture** is a module name and a lecture name, for example `EN2090` and
`Lecture 4`. Every card tapped after you start one is recorded after that
lecture's marker in the log, which is how the computer knows which lecture it
belongs to. Start one from the app (Lectures page), or by editing the `#MODULE` and
`#LECTURE` lines in `SETTINGS.CSV` and ejecting the drive (or tapping the button,
or unplugging). Edit them to the same text as
before and add a line `#NEWSESSION,1` to start a second lecture with the same
names.

### Starting a lecture on the device

You can take attendance for several lectures in a row without a computer.
When one lecture ends and the next begins, **hold the power button until you
feel a buzz (2 seconds), then let go.** The device answers with a green light and
three quick buzzes, and every card counts again from that moment.

The new lecture keeps the module of the one before and counts its name on:
`Circuits Lecture 4` becomes `Circuits Lecture 5`, then `Circuits Lecture 6`. A
name without a number gets ` 2`, ` 3` and so on, and if no lecture was ever set
the first one is `Lecture 1` with no module. The next time you plug the device
in, the companion app adds each of these lectures with the taps that belong to
it; rename one there if the automatic name is not right. A lecture without a
module is filed under **UNASSIGNED** until you move it.

Keep holding past the buzz and the device switches off instead (at 5 seconds),
without starting a lecture. A hold while the device is a USB drive on a computer
only ends the drive session, like a tap; hold again once it is taking attendance.

### Double taps

Within one lecture a card counts **once**. A student who taps twice by accident,
holds the card on the reader, or comes back to the reader later gets a double
buzz and no second row, even if the device has gone to sleep in between. Start
the next lecture and everyone counts again. If you never start a lecture, a card
already recorded in the last six hours still counts once.

## Deleting the records

Starting a lecture from the companion app also empties the device. The app first
reads every tap on the device into its own database, checks that it got all of
them, and only then tells the device to delete its copy (the `#CLEARLOG,1` line
in `SETTINGS.CSV`). The old taps stay in the app, matched to their lectures. If
the app could not read every tap it does not ask, and the device keeps them.

**Clear device records** in the app does the same without starting a lecture:
it reads everything in first, and refuses to clear the device if it could not.

If the device's records cannot be read at all, **Erase device without saving…**
(in the app's Device menu) deletes them without copying them first.
You have to type `ERASE` to confirm. Any taps and lectures that were not already
in the app are lost for good. The device keeps its ID and its clock.

Without the app, add a line `#CLEARLOG,1` to `SETTINGS.CSV` and eject. **Copy
`ATTEND.CSV`, `LECTURES.CSV` and the `LECTURES` folder somewhere safe first:**
the records cannot be brought back. Renaming the lecture by hand never deletes anything.

## Reading the attendance

1. Plug the device into a computer with the USB-C cable.
2. A drive called **ATTENDANCE** appears. Open `ATTEND.CSV` (it opens in Excel,
   Numbers, Google Sheets or any text editor).

```
DATE,TIME,CARD_ID
2026-10-06,09:30:12,0000123456
2026-10-06,09:30:40,0000999999
```

Copy the file somewhere else to keep it. The drive itself is read-only for this
file, so you cannot damage the log by accident. The header row has spaces after
it to keep every row the same width; most programs ignore this, and a script
should strip them. The file holds only taps: the lecture markers are not rows,
and there are no names. To see names and per-lecture attendance, use the
companion app, which matches the card numbers to your student list.

`LECTURES.CSV`, also read-only, lists every lecture start in the log, one row
each: date, time, module and lecture name, for example
`2026-10-07,14:00:03,EN2090,Circuits Lecture 5`. The rows are padded with spaces
like `ATTEND.CSV`. A tap belongs to the last lecture that started before it.

The `LECTURES` folder holds the same taps split up, one read-only file per
lecture, with the same columns as `ATTEND.CSV`:

```
LECTURES
  L000_2026-10-06_09-12.csv    taps from before the first lecture, if there were any
  L001_2026-10-06_14-00.csv    lecture 1, started 2026-10-06 at 14:00
  L002_2026-10-07_14-00.csv
```

The name is the lecture's number on the device, then the date and time it
started (some older programs show only the short name, such as `L001.CSV`).
`L000` is named after its first tap. The numbers start from 1 again after the
records are deleted. To see the lecture's module and name, look up its row in
`LECTURES.CSV`.

`STATUS.TXT` on the drive is a short read-only summary: the device ID, the
clock, the battery, how many records the log holds, how many files the
`LECTURES` folder has, the last tap, the current lecture and when it started,
and what the device will do with your `SETTINGS.CSV` when you eject the drive.
The battery reads like `87 % (3950 mV)`; it reads a little high while the
device is charging, which it is whenever it is plugged in. The companion app
shows the same figure at the top of its window.

`LASTCARD.TXT` shows the card last tapped since the device was plugged in, for
registering cards (see below). The companion app reads it for you.

## Changing the device's settings

1. Plug the device in and open the **ATTENDANCE** drive.
2. Open `SETTINGS.CSV` in a text editor. It is a few lines, each starting with `#`:

   ```
   # Edit these lines, then eject the drive (or press the button). Add #NEWSESSION,1 to start another lecture with the same names.
   #TIME,2026-10-06 14:30:00
   #MODULE,EN2090
   #LECTURE,Circuits Lecture 4
   #DEVICE,0000012345
   ```

3. Change what you need and save, keeping the name `SETTINGS.CSV`. Or replace
   the whole file with your own, saved under exactly that name: a file with any
   other name is ignored. Use a plain text editor (Notepad, TextEdit in plain
   text mode); spreadsheet programs can change the `#TIME` format or the
   encoding, and the device then ignores those lines.
4. **Eject the drive** (or tap the button on the device, or unplug the cable).
   The device checks the file, then shows the result:
   - green light and two buzzes: accepted and applied
   - red light and three buzzes: refused, and nothing was changed

What each line does:

| Line | Effect |
|---|---|
| `#TIME,2026-10-06 14:30:00` | sets the clock, if you changed it (an untouched line leaves the clock alone) |
| `#MODULE,...` and `#LECTURE,...` | name the lecture; a new one starts if either changed |
| `#NEWSESSION,1` | starts a new lecture even if the names are the same |
| `#CLEARLOG,1` | deletes every record on the device (copy `ATTEND.CSV` first) |
| `#DEVICE,...` | sets the device ID printed on the enclosure (the line appears once an ID is set) |

Safe to know:

- Nothing is applied until you eject the drive, tap the button or unplug the
  cable. Save the file first. Holding the power button for 5 seconds while
  plugged in also applies your changes before the device switches off.
- Module names can be up to **24 bytes** and lecture names up to **32 bytes**
  (about that many English letters; fewer for Sinhala or Tamil, which take three
  bytes a letter). A comma inside a name is turned into a space.
- Lines that do not start with `#` are ignored, and so are `#` lines the
  device does not know. That includes the `#CARDS` list an older version of the
  app wrote, with the numbers under it: the device keeps no card list now.
- Read `STATUS.TXT` on the drive to see what the device thinks of your file
  *before* you eject: it says whether the clock, a new lecture, deleting the
  records or a new device ID will be applied, or names the problem. (Your
  computer may show an old copy of `STATUS.TXT` right after you change the file.)

## Setting the clock

The device keeps time while switched off, but it needs setting once, and again
if the battery goes completely flat. Edit the `#TIME` line in `SETTINGS.CSV` to
the current local time, for example `#TIME,2026-10-06 14:30:00`, save, and
eject. (If you leave that line alone the clock is not touched.) The app can do
this for you: it sets the clock whenever you start a lecture, or with **Set the
device clock**.

## Registering a new card

The device does not need to know about a new card: it records every card. The
companion app is where a card is given to a student, and the device can read the
card for it while it is plugged in:

1. Plug the device into the computer and open the companion app.
2. On the **Students** page press **Tap a card…** (or, in a student's window,
   **Tap card on device…**).
3. Tap the card on the device. It shows a green light and buzzes, and the card's
   number appears in the app. The tap is not recorded as attendance.
4. Fill in the index number and name, and save.

With a student picked in the list first, **Tap a card…** gives that student the
new card instead (a lost or replaced card). A card that already belongs to
another student is refused, and the app waits for another one. When you are
done, eject the drive (or tap the button) and the device takes attendance again.

A card can also be registered afterwards: tap it with the device unplugged (it
is recorded like any other), plug the device in, and press **Register…** next to
it in the orange *New cards* box on the Students page.

Without the app there is nothing to register on the device. Find the card's
number in the `CARD_ID` column of `ATTEND.CSV`, or tap it while the device is
plugged in and open `LASTCARD.TXT` (your computer may show an old copy of that
file; close it and open it again), and keep your own list of who owns it.

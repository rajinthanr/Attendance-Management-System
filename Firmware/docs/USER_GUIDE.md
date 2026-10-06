# Card Attendance System: user guide

Written for: the people who run a class with the device, not the firmware team.

The device logs a card tap with the date and time, and shows its log to a
computer as a small USB drive. It records only the card number and the time: it
does not know who owns a card, only which card numbers are registered. For the easiest way to use it, run the companion
app in the `Companion` folder (see its README): it keeps the student list, finds
the device, starts lectures, shows who was present and prints reports. This guide
describes the device itself, which also works with no app at all, by opening the
files on the drive.

## Taking attendance

Tap a card on the reader. The device answers:

| You see | It means |
|---|---|
| green light and one short buzz | a registered card: the tap is recorded |
| red light and one long buzz | a card that is not on the device's list: the tap is still recorded, so you can register the card (see below) |
| two short buzzes | this card is already recorded in this lecture (or was read twice in a row); not recorded again |
| red light blinking five times | the battery is low |

Every card is recorded, registered or not; the colour only tells you whether the
device has that card number on its list. If the device has never been given a
list, every card shows green. Who the card belongs to is worked out later, on the
computer.

After three minutes without a tap the device switches itself off to save the
battery. Press the power button to wake it.

## Lectures and double taps

A **lecture** is a module name and a lecture name, for example `EN2090` and
`Lecture 4`. Every card tapped after you start one is recorded after that
lecture's marker in the log, which is how the computer knows which lecture it
belongs to. Start one from the app (Lecture tab), or by editing the `#MODULE` and
`#LECTURE` lines in `SETTINGS.CSV` and unplugging. Edit them to the same text as
before and add a line `#NEWSESSION,1` to start a second lecture with the same
names.

Within one lecture a card counts **once**. A student who taps twice by accident,
holds the card on the reader, or comes back to the reader later gets a double
buzz and no second row, even if the device has gone to sleep in between. Start
the next lecture and everyone counts again. If you never start a lecture, a card
already recorded in the last six hours still counts once.

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

`STATUS.TXT` on the drive is a short read-only summary: the device ID, the clock,
how many records the log holds, the last tap, how many cards are registered, the
current lecture and when it started, and what the device will do with your `SETTINGS.CSV` when you unplug.

## Changing the device's settings

1. Plug the device in and open the **ATTENDANCE** drive.
2. Open `SETTINGS.CSV` in a text editor. It is a few lines, each starting with `#`:

   ```
   # Edit these lines, then unplug the cable. Add #NEWSESSION,1 to start another lecture with the same names.
   #TIME,2026-10-06 14:30:00
   #MODULE,EN2090
   #LECTURE,Circuits Lecture 4
   #DEVICE,0000012345
   #CARDS,3
   0000000123
   0000000456
   0000000789
   ```

3. Change what you need and save. Or replace the whole file with your own (any
   file name ending in `.csv` also works, as long as it is the only extra one on
   the drive).
4. **Unplug the cable.** The device checks the file, then shows the result:
   - green light and two buzzes: accepted and applied
   - red light and three buzzes: refused, and nothing was changed

What each line does:

| Line | Effect |
|---|---|
| `#TIME,2026-10-06 14:30:00` | sets the clock, if you changed it (an untouched line leaves the clock alone) |
| `#MODULE,...` and `#LECTURE,...` | name the lecture; a new one starts if either changed |
| `#NEWSESSION,1` | starts a new lecture even if the names are the same |
| `#DEVICE,...` | sets the device ID printed on the enclosure |
| `#CARDS,<n>` | the registered card numbers: the next `n` lines, one number each (decimal or `0x` hex), in ascending order, with exactly `n` of them and at most 1000. Leave the line out to keep the device's list; `#CARDS,0` clears it |

Safe to know:

- Nothing is applied until you unplug. Ejecting in the operating system is a good
  habit, but the cable coming out is what applies it.
- Module names can be up to **24 bytes** and lecture names up to **32 bytes**
  (about that many English letters; fewer for Sinhala or Tamil, which take three
  bytes a letter). A comma inside a name is turned into a space.
- Lines that do not start with `#` are ignored, except the card numbers after a
  `#CARDS` line. If that list is out of order, has a zero or a repeat, or does not
  have exactly `n` numbers, the whole file is refused (red, three pulses) and
  `STATUS.TXT` says which line is wrong.
- Read `STATUS.TXT` on the drive to see what the device thinks of your file
  *before* you unplug: it says whether the clock, a new lecture or a new device
  ID will be applied, or names the problem. (If you just changed the file, unplug
  and plug in again to refresh your computer's copy of it.)

## Setting the clock

The device keeps time while switched off, but it needs setting once, and again
if the battery goes completely flat. Edit the `#TIME` line in `SETTINGS.CSV` to
the current local time, for example `#TIME,2026-10-06 14:30:00`, save, and
unplug. (If you leave that line alone the clock is not touched.) The app can do
this for you: it sets the clock whenever you start a lecture, or with **Set the
device clock**.

## Registering a new card

The device cannot tell you whose card it is, and its reader is switched off while
it is plugged in, so a card cannot be registered live:

1. Tap the new card on the device. It is recorded like any other, and shows red
   because the device does not know it yet.
2. Plug the device in and open the companion app. The card appears under *New
   cards* (Students tab).
3. Press **Register** and fill in the name, student number, department and
   modules.
4. Press **Send cards to device**. (The app shows a banner, "The device does not
   have your latest student cards", until you do. Starting a lecture sends the
   list as well.)
5. Unplug the cable. The device checks and stores the list: green and two buzzes
   means it was accepted. From now on the card shows green.

Without the app, find the number in the `CARD_ID` column of `ATTEND.CSV`, add it
to the numbers under `#CARDS` in `SETTINGS.CSV` (keep them in ascending order and
fix the count), and unplug.

# Card Attendance System — firmware architecture

Target: **STM32L432KCU6** (Cortex-M4F, 256 kB flash, 64 kB SRAM, UFQFPN32).
The 128 kB STM32L432KBU6 cannot run this firmware as it is: the card list and
the log live in the upper 128 kB (see "Board notes").

## The two levels

The firmware is split along one rule: **Level 2 decides, Level 1 acts.**

```
                    App/            Level 2 — logic and arithmetic
                     |                no HAL, no vendor headers, no registers
                     |                compiles and runs on a workstation
       platform_if.h |  <-- the contract
                     |
                    Bsp/            Level 1 — HAL drivers
                                      no policy, no arithmetic, no decisions
                                      moves bytes and toggles pins
```

`App/Inc/platform_if.h` is written from Level 2's point of view: it *declares*
what the application needs, and `Bsp/Src/*.c` *implements* it. The dependency
arrow therefore points from the hardware towards the logic, not the other way
round, which is what lets `App/` build against a RAM-backed stub on the host.

### What that buys

`Tests/` compiles every Level 2 module against a RAM-backed stub platform with
a simulated ISO14443-A card, and runs about 780 assertions in under a second:
card activation with 4-, 7- and 10-byte UIDs, presence tracking, button
debouncing, exhaustive calendar round trips from 2000 to 2099, the flash log's
recovery from a power loss mid-page, the FAT12 image a host has to accept, a
simulated PC that copies and edits `SETTINGS.CSV` over that image, the card
list's storage, CRC and known/unknown feedback, and the whole state machine
driven end to end on simulated time, from card tap to unplug, lecture sessions
and duplicate taps included. None of that needs hardware, a debugger, or a
card.

```
make test          # Level 2, on the host
make               # the full firmware, cross compiled
```

### The rules, concretely

Level 1 never returns a cooked value:

| Level 1 hands up | Level 2 turns it into |
|---|---|
| ADC counts + `VREFINT_CAL` | millivolts, then a battery verdict (`battery.c`) |
| the bytes a card sent (FIFO contents) | a checked UID (`iso14443a.c`), then a card ID (`card_reader.c`) |
| raw button and VBUS levels | debounced presses and attach/detach events (`button.c`, `app_fsm.c`) |
| RTC calendar fields | seconds since epoch, and back (`timeutil.c`) |
| a 512-byte sector request | a synthesised FAT12 / CSV sector (`usb_storage.c`) |

Level 1 never makes a decision. There is no `plat_handle_card()`. The only
Level 2 symbol Level 1 calls is `app_event_post()`, from interrupt context
(today only the USB storage callback does).

The reader's supply mode is an example of where the line falls. Whether the
ST25R3916 runs in its 3.3 V or 5 V mode depends on the battery voltage, so
Level 2 decides (`nfc_supply_3v3_for()`, with hysteresis) and passes a bool to
`plat_nfc_init()` / `plat_nfc_set_supply()`. The no-response timeout and the
SPI prescaler are properties of the chip and the clock tree, so they live in
`bsp_board.h`. That is the test to apply when the boundary is unclear.

## Layout

```
App/Inc, App/Src      Level 2. app_fsm, card_reader, iso14443a, button,
                      log_store, device_cfg, record_buffer, dedup, feedback,
                      battery, csv, fat12, settings_file, session,
                      usb_storage, timeutil, crc, app_events, app_debug
Bsp/Inc, Bsp/Src      Level 1. bsp_clock/gpio/time/nfc/flash/adc/power/usb,
                      bsp_isr, plus usbd_conf and usbd_desc
Core/                 CubeMX output. main.c calls bsp_init() then app_init(),
                      inside USER CODE blocks, so regeneration is safe.
Drivers/              STM32L4 HAL and CMSIS
Middlewares/          ST USB Device Library, core + MSC class
Tests/                host build of Level 2
```

Peripheral interrupt handlers live in `Bsp/Src/bsp_isr.c`, **not** in
`Core/Src/stm32l4xx_it.c`, so that regenerating from the `.ioc` cannot
clobber them.

### The `.ioc`

`Card Attendance System.ioc` carries the full pin map — every signal, label
and pull — so the CubeMX pinout view matches the board and a pin conflict is
caught there rather than on the bench. It still declares the button, VBUS and
reader IRQ as EXTI inputs, and TIM1/TIM2 from the 125 kHz design; the BSP
de-initialises those pins and reconfigures them as plain inputs, and never
starts the timers. Bring the `.ioc` in line when it is next regenerated.

The interrupts the BSP owns are listed with code generation switched off, so a
regeneration cannot emit a second copy of a handler that already lives in
`bsp_isr.c`. The USB *middleware* is deliberately not declared — only the USB
peripheral — because the MSC class would generate its own `usbd_conf.c` and
`usbd_desc.c` beside the ones in `Bsp/Src`.

Level 1 stays authoritative: it configures every peripheral at run time and
reads nothing CubeMX generates. Regenerating adds `MX_*_Init()` calls to
`main.c` that are redundant rather than harmful — `bsp_init()` runs after them
and reprograms the same registers, at the cost of about 1 kB of RAM in
duplicate handles.

### After regenerating from the `.ioc`

CubeMX prunes `Drivers/` and `Middlewares/` down to what the `.ioc` declares,
and rewrites `.cproject`. Two things it gets wrong for this project:

1. **`Middlewares/ST/STM32_USB_Device_Library/` is deleted**, and `Middlewares`
   is dropped from the `.cproject` source folders (the include paths survive,
   which makes the failure look like a link error rather than a missing tree).
   Restore the library from `STM32Cube_FW_L4` — Core and Class/MSC, without the
   `*_template.c` files — and re-add the source-folder entry.
2. **`HAL_PCD_MspInit` / `HAL_PCD_MspDeInit` are re-emitted** into
   `stm32l4xx_hal_msp.c` and collide with the ones in `bsp_usb.c`. Delete the
   generated pair; a note in that file's `USER CODE BEGIN 1` block survives
   regeneration and says so. The BSP's version is the one that matters: it uses
   HSI48 + CRS rather than PLLSAI1, enables VDDUSB, sets the NVIC priority, and
   parks PA11/PA12 as analog on unplug.

The clock tree in the `.ioc` is MSI at 4 MHz with the PLL off, matching
`bsp_clock.c`. If it ever drifts back to the 80 MHz PLL default, the generated
`SystemClock_Config()` will start that PLL before `bsp_init()` runs;
`stop_unused_oscillators()` shuts it down again, but the `.ioc` is the place to
fix it.

## Power design

The firmware currently runs in **polling mode**, to make bring-up and
debugging simple. Interrupts and Stop 2 come back once the hardware is proven.

| Mode | Used when | Retained | Wakes on |
|---|---|---|---|
| Sleep | between main-loop passes, always | everything | SysTick (1 ms), USB |
| Standby | 3 min idle on battery, low battery, a 5 s hold | RTC + backup registers | WKUP1 (button) only, through reset |

`app_task()` runs once per millisecond: it samples the button and VBUS, steps
the feedback pattern, polls the reader, then sleeps in WFI until the next
SysTick. Every timer is a timestamp compared against `plat_uptime_ms()`
(`HAL_GetTick()`), so nothing blocks, not even the reader's 5 ms field guard.

The reader polls every 100 ms. Each poll switches the field on, waits the
ISO14443 guard time, runs REQA/anticollision/SELECT, and switches the field
off again, so the field is on about 6 % of the time. The ST25R3916 stays in
Ready mode (oscillator running) between polls and is put into power-down
before Standby, since it runs straight off the cell.

Consequences worth stating:

- **Feedback never blocks.** Patterns are tables of (output mask, duration)
  stepped against the uptime. The reader pauses while one plays, so the motor
  never runs with the field on, and a card held through the pattern is not
  reported twice.
- **Flash is written in batches, and soon.** Records stage in RAM and go to
  flash at 80 % occupancy, or 5 s after the last scan, whichever is first.
  Appending is a few double-word programs; erases happen once per 254 records.
- **The ADC is powered down between samples** (every 10 s), and re-calibrated
  on each power-up because it has to be.
- **Spare pins are analog**, the lowest-leakage state on an L4.
- **The core runs at 4 MHz** while scanning. USB sessions raise it to 24 MHz
  and drop back on unplug.
- **The button's pull-up is retained in Standby** via `PWR_PUCRA`. Without
  that, PA0 floats and the unit wakes on noise. Standby is only entered once
  the button is released, so the press that switched the unit off cannot
  switch it straight back on.

### Moving to interrupts

The event queue (`app_events.c`) is already the seam. The polled sources post
the same events an ISR would: `APP_EVT_BUTTON_*` from `poll_button()`,
`APP_EVT_USB_*` from `poll_vbus()`, `APP_EVT_INACTIVITY` from `run_idle()`.
The plan:

1. Button and VBUS on EXTI, posting raw edges; keep the debounce in Level 2.
2. LPTIM1/LPTIM2 on the LSE for the inactivity and pattern timers, so
   `plat_uptime_ms()` no longer depends on SysTick.
3. The ST25R3916 wake-up mode (amplitude/phase/capacitive measurement on its
   own RC timer) in place of the 100 ms poll, waking the MCU on PB1.
4. Then Stop 2 between events. `plat_sleep_idle()` already re-tests its
   predicate with interrupts masked, which closes the race between "queue is
   empty" and the WFI.

## Data formats

### Attendance record — 8 bytes

```c
struct { uint32_t student_id; uint32_t stamp; };   /* card ID (the field name is historical); epoch 2000-01-01 */
```

Exactly one STM32L4 flash double-word, the smallest programmable unit. Record
size and programming granularity being identical removes read-modify-write
from the log entirely: a power loss can only lose the record being written,
never corrupt one already stored.

### Flash map — top 128 kB (`0x08020000`)

```
page  0        config: device ID, card count, card-list CRC-32
pages 1..8     registered card list: sorted uint32 IDs, up to 1000 (flash has room for 4096)
pages 9..63    attendance log, 55 pages x 254 records = 13 970 records
```

The linker script's `FLASH` region was shortened to 128 kB and an `NVDATA`
region added, so an image that would overlap the log fails to link instead of
erasing records at run time.

The config page is one 32-byte `nv_config_t`: magic `"CAS1"`, `format_version` 4,
`device_id`, `card_count`, `card_crc32` (CRC-32/IEEE over the IDs as
little-endian words) and reserved words. `devcfg_set_device_id()` erases the page,
programs it and reads it back, keeping the card list. A blank page, or one
written by an earlier layout (format 1 to 3), reads as "no configuration" with
device ID 0 and no cards; it is never interpreted. A card count out of range
reads as no list.

The card list is `card_count` sorted 32-bit card numbers (no names) in pages 1..8,
at most `NV_CARDS_MAX` = 1000, searched in place by binary search
(`cards_is_known()`). `devcfg_set_cards()` replaces it: the card pages and the
config page are erased first, the IDs are programmed, the config page is
written last and the whole list is read back and checked against its CRC, so a
power cut leaves "no config", never a half-written list that looks valid. A list
identical to the stored one causes no erase. At boot the list is checked against
its CRC once (`cards_verify()`); one that fails is ignored.

Each log page is self describing: a header double-word carrying a monotonic
sequence number, 254 record slots, and a footer written at close carrying the
count and a CRC-16. Logical order comes from the sequence number, not from
physical position. `log_init()` adopts an opened-but-unsealed page and
continues appending, which covers both a power loss mid-write and an ordinary
power-off. Pages are sealed only when they fill: sealing at every power-off
or USB attach, as earlier firmware did, left the rest of the page unusable and
filled the 55-page log after 55 power cycles. There are tests for both.

When the log fills, the device **refuses further records and complains** rather
than overwriting the oldest. Losing attendance history silently is worse than
a device that visibly needs emptying. `APP_LOG_WRAP_WHEN_FULL` inverts this.

### Who owns a card

The device knows nobody's name. It stores a card ID and a time stamp for every
tap, and keeps one list of registered card numbers (numbers only) so that it can
tell a registered card from one it has never been told about. Every card that is
read, and is not a duplicate, is recorded either way:

| Tap | Feedback |
|---|---|
| registered card | green and one short buzz (`FB_ACCEPTED`) |
| card not on the list | red and one long buzz (`FB_UNKNOWN`, `APP_FB_UNKNOWN_VIB_MS`), still recorded |
| duplicate | two short buzzes, not recorded |

A low battery still overrides all of these. With no list (never sent, cleared, or
failing its CRC at boot) every card counts as known, so a device that was not
given a list never shows red. Names, student numbers, departments and module
enrolment live in the PC app's database (`Companion/`), which sends the device
the list of numbers and finds a card's owner by its number.

### Lecture sessions in the log

A lecture is a module name and a lecture name that apply to every attendance
record after it, up to the next one. It is written into the log itself, as a
marker made of ordinary 8-byte records, so it needs no flash area of its own and
lives exactly as long as the attendance it describes:

```
header  {0xFFFFFFF0 | n, start stamp}      n = text records that follow, 0..15
text    {0xFFFFFFD0,     4 name bytes}     "module NUL lecture NUL", NUL padded
```

Card IDs from 0xFFFFFF00 up are reserved (`NV_ID_RESERVED_MIN`; the PC app also
refuses them), so a marker is recognised from its first word, scanning in either
direction. A longest-name marker is 16 records. Module names are at most 24 bytes
and lecture names 32.

A marker is started from the USB drive (see below). `usbs_end()` returns the
names; once USB has stopped, the state machine appends the marker with
`start_session()`, which flushes the RAM buffer first so the marker always fits
and last so it is in flash at once.

A marker is also started from the button, with no PC: hold past
`APP_BTN_LONG_MS` (2 s, a short buzz) and let go before `APP_BTN_OFF_MS` (5 s).
`new_lecture()` in `app_fsm.c` finds the newest marker with `sess_latest()` and
appends one with the same module and the next name from `sess_next_name()`: a
trailing number counts up (`Circuits Lecture 4` -> `Circuits Lecture 5`, `Week
09` -> `Week 10`), a name without one gets ` 2`, an empty log gives `Lecture 1`
with no module, and the text before the number is cut, at a character boundary,
if the result would pass 32 bytes. It is stamped when the button is released.
With fewer than `SESS_MAX_RECORDS` free log slots it shows the log-full pattern
instead. The PC learns of these lectures from `LECTURES.CSV`.

A marker can be torn by a power cut. A header then promises more text than
follows, or the tail of one has lost its header. `usbs_begin()` claims only the
text records that are really there, and records strays separately, so what comes
after is still attendance.

### Attendance CSV: 32 bytes per row, header included

```
DATE,TIME,CARD_ID<spaces>
2026-09-10,13:27:45,0000123456
```

Fixed width, and 32 divides 512 exactly: sixteen rows per sector, no row ever
straddling a sector boundary. A mass-storage host reads sectors in whatever
order it likes, and this turns "which records are in sector N?" into a multiply
(plus a binary search over the session markers, below): no scan, no
RAM-resident copy of the log. Columns: date 0..9, time 11..18, card 20..29, CRLF
30..31. The card ID is zero padded to ten digits; there is no name, module or
lecture column. No column holds a comma or a quote, so no quoting is needed.

Markers are not rows. At attach the log is read once and every marker is noted
in a table (position, attendance records before it, marker records so far; 6
bytes each, up to 768, in SRAM2). Row `r` is then log record `r` plus the marker
records before it, found by binary search. Every marker's names and start time
reach the PC through `LECTURES.CSV`, and the newest one's through `STATUS.TXT`
too (below), not through the attendance CSV.

### Lecture list: 128 bytes per row

```
DATE,TIME,MODULE,LECTURE<spaces>
2026-10-07,14:00:03,EN2090,Circuits Lecture 5<spaces>
```

`LECTURES.CSV` has one row per marker header in the log, in log order, built
from the same marker table (`read_lectures_sector()` in `usb_storage.c`, rows
from `csv_lecture_row()`). The names need more than 32 bytes, so the rows are
128: at most 77 bytes of text, spaces to byte 125, CRLF, four rows to a sector.
A stray text record (the tail of a torn marker) is no lecture and has no row; a
torn header shows the names it still has. The module field is empty for a
lecture started from the button before any was named. Names never hold a comma
or a quote. The PC treats each row as a lecture start by the device clock, and
a tap belongs to the last lecture that started before it.

### Duplicate taps

Three layers, cheapest first:

1. **The reader** accepts a card only after a clean ISO14443-A activation (BCC
   and CRC_A checked in `iso14443a.c`), and `card_reader.c` reports a card held
   on the reader once, not on every 100 ms poll, so one noisy read is never a
   tap.
2. **`dedup.c`**: a card seen in the last 10 seconds is a duplicate, whatever
   else happened. This is the card held against the reader. It is a RAM table,
   so it forgets at Standby.
3. **`sess_card_seen()`** (`session.c`): a card already recorded in the current
   lecture is a duplicate, however long ago. It scans the RAM buffer and then the
   flash log backwards from the newest record and stops at the first session
   header, at a record older than `APP_SESSION_MAX_AGE_S` (6 hours, which is what
   makes it work when no lecture has been started), or after
   `APP_SESSION_SCAN_MAX` records. It reads flash, so a student who comes back
   after the unit has slept is still caught, and it costs a few hundred
   microseconds. A stamp from the future (the clock was set backwards) counts as
   just now.

A duplicate buzzes twice, records nothing, and counts in `scans_duplicate`.
Starting the next lecture resets layer 3 for everyone, because the new header is
where the scan stops.

### USB volume

A FAT12 volume with four files, sized afresh at every attach to be exactly as
large as its contents:

| File | Access | Source |
|---|---|---|
| `ATTEND.CSV` | read-only | synthesised sector by sector from the flash log |
| `SETTINGS.CSV` | read/write | the current settings rendered as text into a 14 kB RAM window (`SETF_MAX_BYTES` = 28 x 512) |
| `STATUS.TXT` | read-only | one generated sector: see below |
| `LECTURES.CSV` | read-only | synthesised from the marker table: one row per lecture start |

```
cluster 2        STATUS.TXT
clusters 3..30   SETTINGS.CSV window (the only free space the host sees)
clusters 31..    ATTEND.CSV
then             LECTURES.CSV (1 cluster per 4 lectures, header included; at most 193)
```

One sector per cluster, two FATs of 6 sectors, 16 root entries; at most 2046
data clusters (the FAT has 2048 entries, so the volume stays far under the 4085
at which a host would read it as FAT16). A full 13 970-record log is about 437 kB
at 32 bytes a row, so it fits with room to spare. The FAT (3 kB) and root
directory are real RAM tables, kept in SRAM2 with the marker table, that the
host's writes modify, so the host may allocate, delete and replace files exactly
as its driver pleases. Because the window is the only free space, everything the
host writes lands in RAM; a write to `ATTEND.CSV`, `LECTURES.CSV` or `STATUS.TXT` is refused, and
a write to the boot sector is accepted and ignored. Windows' `System Volume
Information` and macOS's `.Trashes` fit in the window beside the settings or are
refused for lack of room; neither can touch the log.

`usbs_begin()` takes the loaded `device_cfg_t` (device ID and card list) so that
`SETTINGS.CSV` and `STATUS.TXT` can show them. The ARM build uses about 22.7 kB
(46 %) of the 48 kB main RAM block and 8 kB (50 %) of the 16 kB SRAM2, and 56 kB
of the 128 kB code region.

The record count is latched at attach. A host that saw the file size change
mid-copy would produce a truncated CSV, so scans arriving during a USB session
stay in RAM and appear on the next attach (the reader is off while USB is
attached, so in practice there are none).

`STATUS.TXT` is 512 bytes of text that starts `ATTENDANCE LOGGER` (the PC app
recognises the device by that line). It lists the device ID, the clock, the
number of records in `ATTEND.CSV`, the last tap (card and time), the current
lecture with the device-clock time it started (`module / lecture (since ...)`, or
`none set`), a `Cards        : N registered (CRC XXXXXXXX)` line (or `none
registered`), and what the host's current `SETTINGS.CSV` would do when the session
ends: unchanged, will be applied (clock set, device ID change, all records
deleted, new lecture, new card list), or an error, in which case nothing is applied. A bad card list reads
`ERROR, the card list is wrong at line N`. The PC compares the card count and CRC
with its own list to tell whether the device is out of date. The PC uses the lecture's start time,
which is the device's own clock, to decide which taps belong to which lecture
whatever its own clock says.

### Editing the settings over USB

`SETTINGS.CSV` as first shown:

```
# Edit these lines, then eject the drive (or press the button). Add #NEWSESSION,1 to start another lecture with the same names.
#TIME,2026-10-06 14:30:00
#MODULE,EN2090
#LECTURE,Circuits Lecture 4
#DEVICE,0000012345
#CARDS,3
0000000123
0000000456
0000000789
```

The device shows its own list back, so a host that edits only the lines it cares
about leaves the rest as they were. Nothing reaches the device while the drive is
mounted. The session ends in one of four ways, and all of them run `usb_leave()`:

- **Eject.** The host sends SCSI START STOP UNIT with START = 0 (Eject in
  Explorer, Finder or a Linux file manager, `eject`, `udisksctl power-off`).
  ST's MSC class handles that command without telling anyone, so `bsp_usb.c`
  registers a copy of the class whose `DataOut` looks at the command block just
  decoded and latches `plat_usb_ejected()`. `run_usb()` waits
  `APP_USB_EJECT_GRACE_MS` (1 s) so the host finishes its side, then leaves.
- **A short press of the button.**
- **A 5 s hold** (switch off). `begin_shutdown()` applies the file before switching off. A 2-5 s hold ends the session like a tap.
- **The cable coming out** (VBUS drops).

After an eject or a press the unit stays on USB power and scans; it sets
`usb_hold_off`, so the drive comes back only once the cable has been unplugged
and plugged in again. While VBUS is present the 3-minute idle switch-off is
suspended: Standby would bring the drive back on the next wake instead of the
reader. `usb_leave()` stops the USB peripheral and calls `usbs_end()`, which finds the file by its exact 8.3 name `SETTINGS.CSV` (a file
saved under any other name is ignored), follows its cluster chain through the
window, and parses it with `setf_scan()`, which never touches flash. Only after
the whole file is accepted is anything applied.

Rules for the file (`settings_file.h` has the full list): CRLF, LF or CR line
ends; UTF-8 BOM skipped; only lines whose first non-blank character is `#` mean
anything and every other line is ignored, so an old file that still lists
students does no harm; a comma or quote inside a name becomes a space and quoted
values work. A file that is empty, larger than the window, or whose cluster chain
is broken is refused.

| Directive | Effect |
|---|---|
| `#TIME,YYYY-MM-DD HH:MM[:SS]` | sets the RTC, but only when it differs from the value the file was shown with, so an untouched file never winds the clock back to the moment of attach |
| `#DEVICE,<id>` | sets the device ID (decimal or `0x` hex), stored in the config page and used as the volume serial |
| `#MODULE,<name>` / `#LECTURE,<name>` | name the lecture (24 and 32 bytes). A new session starts when either differs from what the file was shown with. A name the file omits keeps its current value, and clearing both ends the lecture |
| `#NEWSESSION` | starts a new session even if the names are unchanged (a second lecture with the same names) |
| `#CLEARLOG` | erases the whole log (`log_erase_all()`), the RAM buffer and the 10 s table, before any new session marker is written. The Companion app sends it with every lecture start, and only after it has read every row of `ATTEND.CSV` into its database and the row count matched `STATUS.TXT`. Renaming the lecture by hand never clears anything |
| `#CARDS,<n>` | the registered card list: the next `n` lines hold one card number each (decimal or `0x` hex; text after a comma is ignored), strictly ascending, non-zero and below `0xFFFFFF00`, exactly `n` of them, `n` at most 1000. Anything else refuses the whole file (`SETF_ERR_CARDS`, with the line number), so a truncated copy can never become the list. No `#CARDS` line leaves the stored list untouched; `#CARDS,0` clears it (every card then counts as known) |

The card list is stored by `devcfg_set_cards()` when the session ends
(`usbs_end()` reports `cards_set` and `card_count`); an identical list costs no
flash erase. The session marker is written only if the import succeeded. A malformed `#TIME`
or `#DEVICE` value is reported on `STATUS.TXT` and ignored; it does not fail the
import.

When the cable comes out (`usb_leave()` in `app_fsm.c`, after the USB peripheral
has stopped) the unit applies the edit, shows the result and carries on
scanning: green and two pulses for applied (`FB_SAVED`), red and three pulses
for refused (`FB_REJECTED`). If the host changed nothing there is no pattern.
A new device ID is read back from flash before success is claimed; a mismatch
shows red.

**Power loss during the apply.** The only flash writes besides the log are the
config page (device ID) and the card pages. The config page is erased first and
written last, so an interruption leaves "no configuration" (no ID, no list, every
card known) until the host sends it again, never a list that half verifies. The marker is appended
to the log like any other record.

### Finding a new card's ID

Tap the card, plug the device in, and read its ID from `ATTEND.CSV`, or let the
PC app do it: it lists cards it has seen that belong to nobody and offers to
register each one. The reader is off while the device is on USB, so a card cannot
be enrolled live. Registering a card takes the whole round trip: tap it on the
device (it shows red and is recorded), plug in, press Register in the app, press
Send cards to device (or start the next lecture, which sends the list too) and
unplug. From then on the card shows green.

## Card reading (ISO14443-A)

```
field on -> 5 ms guard -> REQA -> ATQA
         -> per cascade level: anticollision (93/95/97 20) -> UID part + BCC
                               SELECT (93/95/97 70 ...)   -> SAK + CRC_A
         -> field off
```

`iso14443a.c` runs the exchange over `plat_nfc_reqa()` and
`plat_nfc_transceive()`, checks the BCC and the SAK's CRC_A itself, and
follows the cascade tag (0x88) through 4-, 7- and 10-byte UIDs. A collision is
reported rather than resolved: the reader expects one card at a time, and the
next poll tries again.

`card_reader.c` turns polls into arrivals. A card held on the reader is seen
by every poll but reported once; it counts as gone after three empty polls in
a row, so a single missed poll does not log it twice. The logged ID is the UID
as a big-endian 32-bit number (`0A F4 1A 9E` is `0x0AF41A9E`, `0183769758` in
the CSV); longer UIDs keep their last four bytes, since the first is the
manufacturer code.

`bsp_nfc.c` drives the ST25R3916 at register level (DS12484 Rev 8). It waits
on the IRQ *pin* rather than polling the status registers over SPI, because
§4.3.3 forbids SPI traffic while a timed direct command runs, and it lets the
chip's no-response timer (1 ms) end each exchange that gets no answer.

## Flow-chart coverage

| Flow chart | Where |
|---|---|
| Start / battery OK? / load list | `app_init()` |
| card present → power RF → read | `run_idle()`, `card_reader.c`, `iso14443a.c`, `bsp_nfc.c` |
| valid ID? (BCC, CRC_A, cascade) | `iso14443a_select()` |
| same ID within 10 s? / already signed in? | `dedup.c` (8-entry MRU table) and `sess_card_seen()`, see "Duplicate taps" |
| known card? | `student_allowed()` → `cards_is_known()` in `handle_card()`: registered gives `FB_ACCEPTED` (green), otherwise `FB_UNKNOWN` (red, long buzz). Either way the card is logged |
| create record, RAM buffer | `handle_card()`, `record_buffer.c` |
| RAM buffer ≥ 80 % | `rb_needs_flush()` → `log_flush()` |
| 3-min inactivity → flush → Standby | `APP_EVT_INACTIVITY` → `begin_shutdown()` |
| USB attach → enumerate → CSV | `usb_attach()`, `usb_storage.c`, `bsp_usb.c` |
| low battery → flush → Standby | `sample_battery()` → `APP_EVT_LOW_BATTERY` |

Branches the flow chart does not have:

- **Every card is recorded.** The card list only chooses green or red; an
  unregistered card is still logged. With no list loaded,
  `APP_ACCEPT_ALL_WHEN_NO_LIST` makes every card green.
- **A charger is not a host.** VBUS that does not enumerate within 5 s is
  treated as a charger: USB is stopped again and scanning carries on.
- **Settings when the USB session ends.** On an eject, a button press or the
  cable coming out, an edited `SETTINGS.CSV` is applied (green, two pulses) or
  refused (red, three pulses), and scanning carries on, with the cable still in
  after an eject or a press.

`handle_card()` checks in this order: the 10 s table (`dedup.c`), then "already
recorded in this lecture" (`sess_card_seen()`), then whether the log has room,
and only then records the card and picks green or red from the card list. So a
repeat tap of an unregistered card gets the duplicate pattern, not red.

Duplicate suppression uses an eight-entry MRU table rather than the single
last-seen slot the diagram implies. With one slot, two people tapping in
alternation each clear the other's entry and both get logged twice; there is a
test for that case.

## User interface

| Event | LEDs | Motor |
|---|---|---|
| Power on | green 300 ms | 120 ms |
| Card accepted | green 250 ms | 90 ms |
| Duplicate (within 10 s, or already in this lecture) | green ×2 | ×2 short |
| Unregistered card (still recorded) | red 450 ms | 450 ms |
| Log full, or reader failed at power-on | red ×4 | ×4 |
| Settings applied (eject, button or unplug) | green, two pulses | ×2 |
| Settings refused | red, three pulses | ×3 |
| Button held 2 s (still down) | — | 60 ms |
| Released between 2 s and 5 s: new lecture | green | ×3 short |
| Button tap | green ×2 (battery OK) or red ×5 (low); while plugged in it first ends the USB session | — |
| Button held 5 s, or 3 min idle on battery | red 700 ms, then off | 250 ms |
| Idle | 30 ms green flash every 4 s; red if the battery is low or the reader failed | — |
| USB session | green flash every second | — |

## Live debugging

The `dbg_*` globals for the STM32CubeIDE Live Expressions view are defined in
`Core/Src/main.c` (the `USER CODE BEGIN PV` block, so CubeMX regeneration
keeps them) and declared in `App/Inc/app_debug.h`: battery millivolts and raw
counts, the last card's UID, ATQA, SAK and logged ID, the scan result, button
state and press counts, reader status, counters and interrupt flags, record
counts, and the RTC. The host build defines its own copies in
`Tests/host_platform.c`.

To set the clock from the debugger, fill in `dbg_set_time` and set
`dbg_set_time_request` to 1. A unit whose RTC was never set starts from the
firmware's build time.

## Companion app

`Companion/` holds the app for the computer the device is plugged into: a
desktop window (`attendance_gui.py`, Python standard library with Tkinter) that
needs no server and no browser except to print. The older browser version
(`attendance_app.py`, serving a page on 127.0.0.1) is still included and shares
the same actions. It recognises the device by
`ATTEND.CSV`, `SETTINGS.CSV` and a `STATUS.TXT` that begins `ATTENDANCE LOGGER`,
reads `ATTEND.CSV` and `STATUS.TXT`, and rewrites only `SETTINGS.CSV`, in place.

Everything the device does not know lives in a SQLite database
(`attendance_db.py`, `~/AttendanceLogger/attendance.db`, with a dated backup once
a day): students (card, name, student number, department, modules), modules,
lectures and every tap. Reading the device adds its taps (known ones are
skipped); attendance is worked out by matching taps to lecture time windows,
using the lecture start times the device reports in `STATUS.TXT`. The app also keeps the device's card list current: it sends the
full list (every student's card number, sorted, never names) inside `SETTINGS.CSV`
with every lecture start and clock set and with the Send cards to device button,
and shows a banner when the count and CRC on `STATUS.TXT` differ from its own
list. It refuses to send more than 1000 cards. Reports come
out as CSV or as PDF (`report_pages.py` makes a page that headless Chrome or Edge
prints; with neither, Print opens the page in the browser, then Save as PDF). `Companion/README.md`
describes use.

## Board notes for the hardware

- **PC14/PC15 need a 32.768 kHz crystal.** Not optional: the RTC runs from
  it, and the move to interrupts puts both LPTIMs on it too.
- **PB7 (`PVD_IN`) must be tied to the battery divider node**, the same node
  as PA7. The PVD's levels 0–6 watch VDD, which a regulator holds steady until
  it drops out entirely — by which point it is too late to write flash. Level 7
  compares PVD_IN against VREFINT and so actually tracks the cell.
- **Never leave PLS at 7 with the PVD off.** In that state PB7 pulls the
  divider node up to VDD and PA7 reads full scale (4079 counts measured on
  the board, against ~1250 when deselected). `pvd_off()` in `bsp_power.c`
  disables the PVD and sets PLS back to 0, at boot and before Standby.
- **The divider is 4.7 M over 2.7 M with 100 nF across the low leg.** It is
  permanently connected because the PVD watches it, so it is sized for ~0.5 µA;
  the cap keeps the source impedance low enough for the ADC's 640.5-cycle
  sampling window.
- **PA0 is the power button**, active low to ground; it is WKUP1, and is
  polled while running.
- USB is crystal-less: HSI48 trimmed by the CRS against the host's SOF.
- **The MCU must be the 256 kB STM32L432KC.** The schematic value and the JLCPCB
  production BOM (`PCB/production/bom.csv`) give U4 as STM32L432KBUx, the
  128 kB part, which has no flash at `0x08020000`. Read the fitted part's flash
  size at `0x1FFF75E0` (256 or 128) before trusting a board.

Full pin map: `Bsp/Inc/bsp_board.h`.

## Known limitations

Found in review on 2026-10-06; none is covered by a test yet. (A long press
while plugged in used to discard the host's edits; `begin_shutdown()` now applies
them first, and `test_fsm_plugged_in()` covers it.)

- **Back-to-back lectures need a new session.** Without a new marker, a card
  recorded in the last 6 hours (`APP_SESSION_MAX_AGE_S`) is a duplicate, so a
  second lecture within 6 hours that was started neither over USB nor with the
  button (hold 2 s, let go) loses every returning student, with no way to
  recover the taps.
- **A button lecture can be started by accident.** One 2-5 s hold starts it,
  with no confirmation. Nothing is lost: the taps after it are filed under the
  new lecture, which can be deleted or merged in the app. While the device is a
  USB drive the hold only ends the drive session.
- **Card IDs from `0xFFFFFF00` up collide with session markers.** The PC app
  refuses them, but `handle_card()` does not: a 7-byte UID whose last four bytes
  fall in that range is logged as a marker and disappears from the CSV.
- **The marker table holds 768 entries.** A log with more lecture markers than
  that is cut short at the 768th in `ATTEND.CSV` and `LECTURES.CSV`, silently.
  With the Companion app's `#CLEARLOG` at every lecture start the log holds the
  markers since the last one, a few lectures started from the button at most, so
  this matters only to a device used without the app.
- **Changing settings erases the config page first.** A power cut between the
  erase and the final write leaves no device ID and no card list (by design no
  half-valid list). After a failed flash write the RAM copy of the config is not
  reloaded, so every card shows red until the next reset.
- **Host editors.** The root directory has 16 entries and the settings window 28
  clusters. An editor that saves to a temporary file and renames it needs a
  second copy's worth of free clusters (a 1000-card list uses 24 of the 28), and
  macOS metadata files use root entries; either can make a save fail with "disk
  full". Excel may also rewrite `#TIME` in a locale format the parser does not
  accept. Edit `SETTINGS.CSV` with a plain text editor, or let the Companion app
  write it.
- **Settings apply when the session ends.** `#TIME` is applied at the eject,
  button press or unplug, so the clock is late by however long the drive stayed
  mounted after the file was written (the app ejects straight away).
- **Eject detection relies on START STOP UNIT.** A host that ejects some other
  way (or only suspends the port) leaves the drive session running; the button
  or the cable still end it. A Windows or Linux host that sends START = 0 for
  disk power management would end the session as well.

## Provisioning

Nothing needs provisioning for the device to log: it records every card it reads.
Without a card list every card shows green; send the list (the app does this with
every lecture start and clock set, or with Send cards to device) to get red for
unregistered cards. Until the RTC is set it starts from the firmware's build
time; set the clock once (the app does it when a lecture is started, or use
`#TIME` in `SETTINGS.CSV`). The device ID is optional and set with `#DEVICE`.
Students are entered in the PC app, which sends the device only their card
numbers. `Tests/fs_check.sh` drives the whole USB path with real FAT tools.

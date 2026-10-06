# Card Attendance System — firmware architecture

Target: **STM32L432KCU6** (Cortex-M4F, 256 kB flash, 64 kB SRAM, UFQFPN32).

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

`Tests/` compiles every Level 2 module with a 140-line stub platform and runs
about 730 assertions in under a second: EM4100 round trips at two data rates and
with jitter, exhaustive calendar round trips from 2000 to 2099, the flash log's
recovery from a power loss mid-page, the FAT12 image a host has to accept, a
simulated PC that copies and edits `SETTINGS.CSV` over that image, the card list's
storage, CRC and known/unknown feedback, and the state
machine driven end to end from card tap to unplug, lecture sessions and duplicate
taps included.
None of that needs hardware, a debugger, or a card.

```
make test          # Level 2, on the host
make               # the full firmware, cross compiled
```

### The rules, concretely

Level 1 never returns a cooked value:

| Level 1 hands up | Level 2 turns it into |
|---|---|
| ADC counts + `VREFINT_CAL` | millivolts, then a battery verdict (`battery.c`) |
| raw capture ticks | a bit period, then a tag ID (`em4100.c`) |
| RTC calendar fields | seconds since epoch, and back (`timeutil.c`) |
| a 512-byte sector request | a synthesised FAT12 / CSV sector (`usb_storage.c`) |

Level 1 never makes a decision. There is no `plat_handle_card()`. The only
Level 2 symbol Level 1 calls is `app_event_post()`, from interrupt context.

Two constants started in `app_config.h` and were moved to `bsp_board.h`
during integration — the carrier frequency and the tank settling time. They
are properties of the antenna, not of the application, and no Level 2 module
referenced them. That is the test to apply when the boundary is unclear.

## Layout

```
App/Inc, App/Src      Level 2. app_fsm, em4100, log_store, device_cfg,
                      record_buffer, dedup, feedback, battery, csv, fat12,
                      settings_file, session, usb_storage, timeutil, crc, app_events
Bsp/Inc, Bsp/Src      Level 1. bsp_clock/gpio/time/rf/flash/adc/power/usb,
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

`Card Attendance System.ioc` carries the full pin map — every signal, label,
pull and EXTI edge — so the CubeMX pinout view matches the board and a pin
conflict is caught there rather than on the bench. The peripheral settings
mirror what Level 1 programs: TIM1 at ARR 31 / CCR 16 for the 125 kHz carrier,
TIM2 prescaled to 1 MHz capturing both edges of CH2 through DMA1_Channel7, the
two LPTIM prescalers, the RTC predividers, and ADC1_IN12 at 640.5 cycles.

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

The flow chart's two sleep states map onto three STM32 modes, because the card
read cannot use the deepest one:

| Mode | Used when | Retained | Wakes on |
|---|---|---|---|
| Sleep | reading a card, USB attached | everything | any interrupt |
| Stop 2 | idle, and between feedback steps | SRAM + registers | touch, VBUS, PVD, LPTIM1/2 |
| Standby | 3 min idle, low battery, button | nothing | WKUP1 only, through reset |

Every timer the design needs in Stop 2 runs from the **LSE**: the RTC for
timestamps, LPTIM1 for the three-minute inactivity window, LPTIM2 for the
short one-shots. A `TIMx` on PCLK would stop with the core clock, which is
why the flow chart's note about TIM1 matters.

Consequences worth stating:

- **Feedback never blocks.** Patterns are tables of (output mask, duration)
  stepped by LPTIM2, so a 450 ms low-battery blink costs one wake per step
  instead of 450 ms of the core spinning in `HAL_Delay`.
- **Flash is written in batches.** A page erase is milliseconds and tens of
  milliamps. Records stage in RAM and go to flash at 80 % occupancy, so that
  cost is paid once per ~102 scans instead of once per scan.
- **The ADC is powered down between samples**, and re-calibrated on each
  power-up because it has to be.
- **Spare pins are analog**, the lowest-leakage state on an L4, across
  sixteen unused pins.
- **The core runs at 4 MHz** while scanning, the slowest MSI range that still
  works at zero flash wait states. USB sessions raise it to 24 MHz and drop
  back on unplug.
- **The button's pull-up is retained in Standby** via `PWR_PUCRA`. Without
  that, PA0 floats and the unit wakes on noise.

### The sleep race

`app_task()` checks its queue, finds it empty, and sleeps. An interrupt landing
between those two steps would leave the device asleep with work outstanding.
`plat_sleep_light()` / `plat_sleep_idle()` therefore take a predicate and
re-test it with interrupts masked, immediately before the WFI. WFI still wakes
on a pending-but-masked interrupt, so masking costs nothing and closes the
window.

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
physical position. An opened-but-unsealed page is unambiguously "power went
away mid-write", and `log_init()` adopts it and continues appending — there
is a test for exactly that.

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
records before it, found by binary search. The newest marker's names and start
time reach the PC through `STATUS.TXT` (below), not through the CSV.

### Duplicate taps

Three layers, cheapest first:

1. **The decoder** needs two matching frames before it accepts a card, so one
   noisy read is never a tap.
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

A FAT12 volume with three files, sized afresh at every attach to be exactly as
large as its contents:

| File | Access | Source |
|---|---|---|
| `ATTEND.CSV` | read-only | synthesised sector by sector from the flash log |
| `SETTINGS.CSV` | read/write | the current settings rendered as text into a 14 kB RAM window (`SETF_MAX_BYTES` = 28 x 512) |
| `STATUS.TXT` | read-only | one generated sector: see below |

```
cluster 2        STATUS.TXT
clusters 3..30   SETTINGS.CSV window (the only free space the host sees)
clusters 31..    ATTEND.CSV
```

One sector per cluster, two FATs of 6 sectors, 16 root entries; at most 2046
data clusters (the FAT has 2048 entries, so the volume stays far under the 4085
at which a host would read it as FAT16). A full 13 970-record log is about 437 kB
at 32 bytes a row, so it fits with room to spare. The FAT (3 kB) and root
directory are real RAM tables, kept in SRAM2 with the marker table, that the
host's writes modify, so the host may allocate, delete and replace files exactly
as its driver pleases. Because the window is the only free space, everything the
host writes lands in RAM; a write to `ATTEND.CSV` or `STATUS.TXT` is refused, and
a write to the boot sector is accepted and ignored. Windows' `System Volume
Information` and macOS's `.Trashes` fit in the window beside the settings or are
refused for lack of room; neither can touch the log.

`usbs_begin()` takes the loaded `device_cfg_t` (device ID and card list) so that
`SETTINGS.CSV` and `STATUS.TXT` can show them. In an ARM build with
`LED_BOOT_DEBUG` 0 the application uses about 53 % of the main RAM block and
50 % of SRAM2.

The record count is latched at attach. A host that saw the file size change
mid-copy would produce a truncated CSV, so scans arriving during a USB session
stay in RAM and appear on the next attach (the reader is off while USB is
attached, so in practice there are none).

`STATUS.TXT` is 512 bytes of text that starts `ATTENDANCE LOGGER` (the PC app
recognises the device by that line). It lists the device ID, the clock, the
number of records in `ATTEND.CSV`, the last tap (card and time), the current
lecture with the device-clock time it started (`module / lecture (since ...)`, or
`none set`), a `Cards        : N registered (CRC XXXXXXXX)` line (or `none
registered`), and what the host's current `SETTINGS.CSV` would do when unplugged:
unchanged, will be applied (clock set, device ID change, new lecture, new card
list), or an error, in which case nothing is applied. A bad card list reads
`ERROR, the card list is wrong at line N`. The PC compares the card count and CRC
with its own list to tell whether the device is out of date. The PC uses the lecture's start time,
which is the device's own clock, to decide which taps belong to which lecture
whatever its own clock says.

### Editing the settings over USB

`SETTINGS.CSV` as first shown:

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

The device shows its own list back, so a host that edits only the lines it cares
about leaves the rest as they were. Nothing reaches the device while the cable is in. When VBUS drops, `usb_detach()`
stops the USB peripheral and calls `usbs_end()`, which finds the file (by name,
or the single other `.csv` if `SETTINGS.CSV` was not touched), follows its
cluster chain through the window, and parses it with `setf_scan()`, which never
touches flash. Only after the whole file is accepted is anything applied.

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
| `#CARDS,<n>` | the registered card list: the next `n` lines hold one card number each (decimal or `0x` hex; text after a comma is ignored), strictly ascending, non-zero and below `0xFFFFFF00`, exactly `n` of them, `n` at most 1000. Anything else refuses the whole file (`SETF_ERR_CARDS`, with the line number), so a truncated copy can never become the list. No `#CARDS` line leaves the stored list untouched; `#CARDS,0` clears it (every card then counts as known) |

The card list is stored by `devcfg_set_cards()` when the cable comes out
(`usbs_end()` reports `cards_set` and `card_count`); an identical list costs no
flash erase. The session marker is written only if the import succeeded. A malformed `#TIME`
or `#DEVICE` value is reported on `STATUS.TXT` and ignored; it does not fail the
import.

When the cable comes out after an edit the unit shows the result before it goes
to Standby: green and two pulses for applied (`FB_SAVED`), red and three pulses
for refused (`FB_REJECTED`). If the host changed nothing it powers down at once.
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

## EM4100 decoding

```
edges -> intervals -> fitted half-bit clock -> half-bit symbols
      -> Manchester bits -> header search -> parity check -> tag
```

The bit clock is **fitted, not assumed** — a two-cluster 1-D k-means seeded
from the shortest interval. The same code therefore reads RF/64, RF/32 and
RF/16 tags and tolerates the antenna pulling the period around by several
percent. Timestamps alone do not say whether the first edge was rising or
falling, so a failed header search retries with the stream inverted.

Two parity-clean frames must agree before an ID is accepted, as the flow chart
requires. A single frame already carries fourteen parity bits; an error would
have to survive them twice and land on the same wrong ID.

The decoder takes a caller-supplied workspace rather than owning static
buffers, so its ~1.5 kB stays under the caller's control and the module has no
hidden state to confuse a test.

## Flow-chart coverage

| Flow chart | Where |
|---|---|
| Start / battery OK? / load list | `app_init()` |
| touch interrupt → power RF → capture | `start_read()`, `bsp_rf.c` |
| valid ID? (parity, 2 frames) | `em4100_decode()` |
| same ID within 10 s? / already signed in? | `dedup.c` (8-entry MRU table) and `sess_card_seen()`, see "Duplicate taps" |
| Known card? | `cards_is_known()` in `handle_tag()`: registered gives `FB_ACCEPTED` (green), otherwise `FB_UNKNOWN` (red, long buzz). Either way the card is logged |
| create record, RAM buffer | `handle_tag()`, `record_buffer.c` |
| RAM buffer ≥ 80 % | `rb_needs_flush()` → `log_flush()` |
| 3-min inactivity → flush → Standby | `APP_EVT_INACTIVITY` → `shutdown()` |
| USB attach → enumerate → CSV | `usb_attach()`, `usb_storage.c`, `bsp_usb.c` |
| PVD low battery → flush → Standby | `APP_EVT_LOW_BATTERY` |

Duplicate suppression uses an eight-entry MRU table rather than the single
last-seen slot the diagram implies. With one slot, two people tapping in
alternation each clear the other's entry and both get logged twice; there is a
test for that case.

## Companion app

`Companion/` holds the app for the computer the device is plugged into. It is a
Python helper (`attendance_app.py`, standard library only) that serves a browser
page on 127.0.0.1; the helper exists because a web page alone cannot list drives
and browsers refuse to open the root of a USB drive. It recognises the device by
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
prints; with neither, use the browser's Print, Save as PDF). `Companion/README.md`
describes use.

## Board notes for the hardware

- **PC14/PC15 need a 32.768 kHz crystal.** Not optional: the RTC and both
  LPTIMs depend on it, and without it nothing survives Stop 2.
- **PB7 (`PVD_IN`) must be tied to the battery divider node**, the same node
  as PA7. The PVD's levels 0–6 watch VDD, which a regulator holds steady until
  it drops out entirely — by which point it is too late to write flash. Level 7
  compares PVD_IN against VREFINT and so actually tracks the cell.
- **The divider is 4.7 M / 2.7 M with 100 nF across the low leg.** It is
  permanently connected because the PVD watches it, so it is sized for ~0.4 µA;
  the cap keeps the source impedance low enough for the ADC's 640.5-cycle
  sampling window.
- **PA0 is the power button**, active low to ground; it is both WKUP1 and EXTI0.
- USB is crystal-less: HSI48 trimmed by the CRS against the host's SOF.

Full pin map: `Bsp/Inc/bsp_board.h`.

## Provisioning

Nothing needs provisioning for the device to log: it records every card it reads.
Without a card list every card shows green; send the list (the app does this with
every lecture start and clock set, or with Send cards to device) to get red for
unregistered cards.
Until a host sets the RTC the calendar starts at 2026-01-01, so set the clock
once (the app does it when a lecture is started, or use `#TIME` in
`SETTINGS.CSV`). The device ID is optional and set with `#DEVICE`. Students are
entered in the PC app, which sends the device only their card numbers. `Tests/fs_check.sh` drives the whole
USB path with real FAT tools.

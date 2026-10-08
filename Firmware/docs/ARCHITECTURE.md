# Card Attendance System — firmware architecture

Target: **STM32L432KBU6** (Cortex-M4F, 128 kB flash, 64 kB SRAM, UFQFPN32).
The code and the attendance data share the 128 kB of flash: code in the first
72 kB, the config page and log in the last 56 kB (see "Flash map").

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
a simulated ISO14443-A card, and runs about 900 assertions in under a second:
card activation with 4-, 7- and 10-byte UIDs, presence tracking, the reader's
wake-up mode, button debouncing, exhaustive calendar round trips from 2000 to
2099, the flash log's recovery from a power loss mid-page, the FAT12 image a
host has to accept (the `LECTURES` folder and its long names included), a
simulated PC that copies and edits `SETTINGS.CSV` over that image, a card read
while plugged in reaching `LASTCARD.TXT`, and the whole state machine driven end
to end on simulated time, from card tap to unplug, lecture sessions, duplicate
taps and the sleep deadlines included. None of that needs hardware, a debugger, or a
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
Level 2 symbol Level 1 calls is `app_event_post()`, from interrupt context: the
USB storage callback, the button and VBUS edges (EXTI0, EXTI9) and the reader's
wake-up (EXTI1).

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
caught there rather than on the bench. It declares the button, VBUS and
reader IRQ as EXTI inputs, and TIM1/TIM2 from the 125 kHz design. The BSP
de-initialises those pins and arms them itself (button and VBUS on both edges,
the reader IRQ rising and masked outside wake-up mode), puts LPTIM1 on the LSE
where CubeMX clocks it from PCLK, and never starts TIM1/TIM2. Bring the `.ioc`
in line when it is next regenerated.

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

The main loop is a deadline loop: each pass does what is due, works out when
it next has to run, and sleeps until then or until an interrupt. Between cards
the reader watches the antenna on its own, and the MCU spends most of its time
in Stop 2.

| Mode | Used when | Retained | Wakes on |
|---|---|---|---|
| Sleep | USB sessions and switching off (1 ms passes), and any wait under `BSP_STOP2_MIN_MS` (4 ms) | everything | SysTick (1 ms), any interrupt |
| Stop 2 | idle (`ST_IDLE`) with the next deadline 4 ms or more away | SRAM, registers, RTC, LPTIM1 | LPTIM1 at the deadline (at most 1.9 s), the button (EXTI0), VBUS (EXTI9), the reader's wake-up (EXTI1) |
| Standby | 3 min idle on battery, low battery, a 5 s hold | RTC + backup registers | WKUP1 (button) only, through reset |

Each `app_task()` pass samples the button and VBUS, steps the feedback pattern,
runs the reader, handles queued events, publishes the `dbg_*` globals, and then
calls `plat_sleep_until(next_wake(), deep, queue_still_empty)`. `next_wake()`
in `app_fsm.c` takes the soonest of the deadlines the loop owns: the feedback
pattern's next step, the heartbeat LED's next edge, the button's debounce and
its 2 s and 5 s thresholds (`btn_next_ms()`), the VBUS debounce, the reader's
field guard or next poll (`cr_next_ms()`), the reader retry, the 10 s battery
sample, the 5 s flush and the 3-minute inactivity timeout, capped at
`APP_SLEEP_MAX_MS` (1 s). `deep` is set only in `ST_IDLE`: a USB session needs
the 24 MHz clock and SysTick, so it and the shutdown sequence keep 1 ms passes.
Every timer is still a timestamp compared against `plat_uptime_ms()`
(`HAL_GetTick()`), so nothing blocks, not even the reader's 5 ms field guard.

`plat_sleep_until()` (`bsp_power.c`) re-tests its predicate with interrupts
masked, so an event posted between the queue check and the WFI is never slept
through. For a deep sleep of at least 4 ms, `stop2_for()` sets an LPTIM1
compare at the deadline and enters Stop 2. LPTIM1 counts the LSE (16 bits,
32768 Hz, wrapping every 2 s) without pause and interrupts on both the compare
and the wrap, through EXTI line 32, so a sleep always ends within one wrap and
the ticks slept are never ambiguous; that is why one sleep is capped at
`BSP_STOP2_MAX_MS` (1.9 s). On waking (on MSI at the scanning range), the ticks
slept are converted to milliseconds and added to HAL's `uwTick`, with the
remainder carried to the next sleep, so `plat_uptime_ms()` never notices the
gap. `BSP_ENABLE_STOP2` in `bsp_board.h` set to 0 brings back Sleep-mode-only
operation. With a debugger attached the core never sleeps, so Live Expressions
reads stay stable.

The button and VBUS interrupt on both edges only to wake the loop:
`bsp_input_edge()` posts `APP_EVT_INPUT_EDGE` once per sleep (a bouncing
contact makes dozens of edges), and the next pass samples the levels and
debounces them in Level 2 as before. An edge that arrives after the loop's last
sample but before the sleep is caught by `bsp_input_changed()`, which compares
the pins with the levels the loop last read.

A reader poll switches the field on, waits the ISO14443 guard time, runs
REQA/anticollision/SELECT, and switches the field off again. With
`APP_NFC_USE_WAKEUP` set (the default) the reader is polled only while
something is in the field; see [Card detection](#card-detection-wake-up-mode).
With it cleared, the reader polls every 100 ms for ever, the field is on
about 6 % of the time, and the ST25R3916 stays in Ready mode (oscillator
running) between polls. Either way it is put into power-down before Standby,
since it runs straight off the cell. The reader also stays on during a USB
session, for registering cards (see below).

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
- **The idle loop wakes at least once a second** (`APP_SLEEP_MAX_MS`), and for
  each edge of the 4 s heartbeat flash. The cap only bounds how stale the
  `dbg_*` globals get.
- **The button's pull-up is retained in Standby** via `PWR_PUCRA`. Without
  that, PA0 floats and the unit wakes on noise. Standby is only entered once
  the button is released, so the press that switched the unit off cannot
  switch it straight back on.

### Moving to interrupts

The event queue (`app_events.c`) was the seam: the polled sources posted the
same events an ISR would, so moving a source to an interrupt changed where the
event comes from and not how it is handled. The plan had four steps, and all
are now done:

1. **Done:** button and VBUS on EXTI0 and EXTI9, both edges. The ISR only wakes
   the loop (`APP_EVT_INPUT_EDGE`); the debounce stays in Level 2, which still
   samples the levels and posts `APP_EVT_BUTTON_*` from `poll_button()` and
   `APP_EVT_USB_*` from `poll_vbus()`.
2. **Done, differently from the plan.** The plan was to move the timers to
   LPTIM1/LPTIM2 so `plat_uptime_ms()` no longer depended on SysTick. Instead,
   SysTick still keeps time while the core runs, and LPTIM1 on the LSE only
   bridges each Stop 2 sleep, adding the time slept to the tick count. Every
   Level 2 timer stayed a timestamp against the uptime, and LPTIM2 is unused.
3. **Done:** the ST25R3916 wake-up mode (amplitude measurement on its own
   RC timer) in place of the 100 ms poll, posting `APP_EVT_NFC_WAKE` from
   EXTI1 (PB1).
4. **Done:** Stop 2 between events, through `plat_sleep_until()` and the
   deadlines from `next_wake()`, above.

Still polled: the battery, sampled on the ADC every 10 s (the PVD on PB7 stays
off, `BSP_ENABLE_BATTERY_PVD`). USB sessions do not use Stop 2.

## Data formats

### Attendance record — 8 bytes

```c
struct { uint32_t student_id; uint32_t stamp; };   /* card ID (the field name is historical); epoch 2000-01-01 */
```

Exactly one STM32L4 flash double-word, the smallest programmable unit. Record
size and programming granularity being identical removes read-modify-write
from the log entirely: a power loss can only lose the record being written,
never corrupt one already stored.

### Flash map — 128 kB, code then data

```
0x08000000  pages  0..35   code, 72 kB      (about 60 kB at -Og, 50 kB at -Os)
0x08012000  page  36       config: device ID              (region page 0)
0x08012800  pages 37..63   attendance log, 27 pages x 254 = 6 858 records
                                                        (region pages 1..27)
```

The linker script (`STM32L432KBUX_FLASH.ld`) gives `FLASH` 72 kB and the
rest to an `NVDATA` region, so an image that would overlap the log fails to
link instead of erasing records at run time. `ASSERT`s in the script check
that the two regions tile the 128 kB, and `_Static_assert`s in `bsp_flash.c`
check `BSP_FLASH_BASE` / `BSP_FLASH_SIZE` against `NV_REGION_BYTES`. -O0
(about 97 kB) does not fit, so the CubeIDE Debug configuration builds at -Og.

Firmware for the earlier 256 kB layout kept its data at `0x08020000`, which the
KB does not specify (it may read back on a given chip, untested). This layout
reads none of it: import a unit's log with the app before reflashing, and set
its device ID again afterwards. Old code left in the region by a reflash is no
valid config (no magic) and no valid log page, and `log_init()` erases it.

The config page is one 32-byte `nv_config_t`: magic `"CAS1"`, `format_version` 4,
`device_id`, and words that are written 0 and ignored (`old_card_count` and
`old_card_crc32`, where the card list's size and CRC used to be, and three
reserved). `devcfg_set_device_id()` erases the page, programs it last
double-word first so the magic goes in last, and reads the whole page back. A
blank page, or one written by an earlier layout (format 1 to 3), reads as "no
configuration" with device ID 0; it is never interpreted. A page left by
firmware that still kept a card list is format 4 too, and loads with its device
ID; its card fields and pages 1..8 are simply never read. The pages stay out of
the log so that the log's pages did not move.

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

The device knows nobody's name and keeps no list of registered cards. It
stores a card ID and a time stamp for every tap, and every card that is read and
is not a duplicate is recorded:

| Tap | Feedback |
|---|---|
| new card | green and one short buzz (`FB_ACCEPTED`), recorded |
| duplicate | two short buzzes, not recorded |
| log full | red ×4 (`FB_ERROR`), not recorded |
| any card during a USB session | green and one short buzz (`FB_ACCEPTED`), shown in `LASTCARD.TXT`, not recorded |

A low battery still overrides all of these. Names, student numbers, departments,
module enrolment and which cards are registered at all live in the PC app's
database (`Companion/`), which finds a card's owner by its number. Earlier
firmware kept the registered numbers and showed red for a card not on the list;
that list, the `#CARDS` directive and the red "unknown card" pattern are gone.

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
too (below), not through the attendance CSV. The `LECTURES` folder splits the
same rows at the markers, one file per lecture.

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

### Lecture files: the `LECTURES` folder

```
LECTURES\L000_2026-10-06_09-12.csv    taps before the first lecture, if any (8.3 alias L000.CSV)
LECTURES\L001_2026-10-06_14-00.csv    lecture 1 (L001.CSV)
LECTURES\L002_2026-10-07_14-00.csv    lecture 2 (L002.CSV)
```

For people reading the drive by hand; the PC app does not use the folder.
`plan_lecture_files()` in `usb_storage.c` makes one file per marker header, in
log order, plus `L000` when attendance records come before the first header (or
the log has taps and no header at all). A file holds the attendance rows from
its marker up to the next file's, in the same 32-byte rows and header as
`ATTEND.CSV` and rendered by the same code (`attend_sector()`), so a file costs
only a 4-byte table entry (its marker and first cluster; up to 769, in SRAM2).
`read_lfile_sector()` finds the file that owns a cluster by binary search.

The name is the lecture number, then the marker's start date and time; `L000`
is named after its first tap. Lecture `n` is row `n` of `LECTURES.CSV`, which
holds its module and lecture names. Numbers count from 1 in log order, so they
start again after `#CLEARLOG`. Stray text records start no file. Each name takes
two long-name entries (`fat12_lfn_entry()`, with `fat12_sfn_checksum()` of the
alias) and the 8.3 alias, dated with the lecture's start. The directory is
generated on every read (`read_dir_sector()`): `.`, `..`, then three entries a
file. The files are placed last and get whatever clusters are left; if they did
not all fit, the oldest would be left out, but with the shipped log size they
always fit. `STATUS.TXT` gives the count.

### Duplicate taps

Three layers, cheapest first:

1. **The reader** accepts a card only after a clean ISO14443-A activation (BCC
   and CRC_A checked in `iso14443a.c`), and `card_reader.c` reports a card held
   on the reader once, not on every 100 ms poll, so one noisy read is never a
   tap.
2. **`dedup.c`**: a card recorded in the last 10 seconds is a duplicate. This
   is the card held against the reader. The 10 s run from the tap that counted:
   a retry inside them does not restart them (it did once, which held off a
   student who kept tapping). Starting a lecture clears the table
   (`start_session()`), so the last card of one lecture counts at once in the
   next; before that fix, a card tapped just before a long press was a
   "duplicate" in the new lecture. It is a RAM table, so it forgets at Standby.
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

A FAT12 volume with five files and a folder, sized afresh at every attach to be
exactly as large as its contents:

| File | Access | Source |
|---|---|---|
| `ATTEND.CSV` | read-only | synthesised sector by sector from the flash log |
| `SETTINGS.CSV` | read/write | the current settings rendered as text into a 14 kB RAM window (`SETF_MAX_BYTES` = 28 x 512) |
| `STATUS.TXT` | read-only | one generated sector: see below |
| `LASTCARD.TXT` | read-only | one generated sector: the card last read during this USB session |
| `LECTURES.CSV` | read-only | synthesised from the marker table: one row per lecture start |
| `LECTURES\` | read-only folder | one CSV per lecture, synthesised like `ATTEND.CSV` (above) |

```
LBA 0            boot sector
LBA 1..12        FAT 1 (12 sectors, 4096 entries)
LBA 13..24       FAT 2 (one RAM copy serves both)
LBA 25           root directory (16 entries)
cluster 2        STATUS.TXT                                  (LBA 26)
clusters 3..30   SETTINGS.CSV window (the only free space the host sees)
cluster 31       LASTCARD.TXT
clusters 32..    ATTEND.CSV
then             LECTURES.CSV (1 cluster per 4 lectures, header included; at most 193)
then             the LECTURES directory
then             the lecture files
```

One sector per cluster and 16 root entries, seven of them the device's own (the
volume label, the five files and the folder). A full log appears twice, in
`ATTEND.CSV` and in the lecture files: 6 858 rows at 32 bytes is about 429
clusters each, about 860 with the lecture list and the directory. The volume
may have at most 4084 data clusters (`FAT12_MAX_CLUSTERS`), the most a FAT12
volume can have before a host reads it as FAT16, so the FAT has 4096 entries,
12 sectors a copy. The FAT (6 kB) and root directory are real RAM tables that the
host's writes modify, so the host may allocate, delete and replace files exactly
as its driver pleases. Because the window is the only free space, everything the
host writes lands in RAM. A write to any generated file is refused; a write to
the boot sector, or to the `LECTURES` directory (a host updating an access
date), is accepted and dropped. Windows' `System Volume Information` and macOS's
`.Trashes` fit in the window beside the settings or are refused for lack of
room; neither can touch the log.

`usbs_begin()` takes the loaded `device_cfg_t` (the device ID) so that
`SETTINGS.CSV`, `STATUS.TXT` and the volume serial can show it. The FAT, the
root directory, the marker table (4.5 kB) and the lecture-file table (3 kB)
live in SRAM2. The ARM build uses about 22.3 kB (46 %) of the 48 kB main RAM
block, 14 kB (88 %) of the 16 kB SRAM2, and 60.3 kB (84 %) of the 72 kB code
region.

The record count is latched at attach. A host that saw the file size change
mid-copy would produce a truncated CSV. Nothing is recorded during a USB
session anyway: a card read then goes to `LASTCARD.TXT` (below), not to the log.

`STATUS.TXT` is 512 bytes of text that starts `ATTENDANCE LOGGER` (the PC app
recognises the device by that line). It lists the device ID (once set), the
clock, the battery (`87 % (3950 mV)` from the last sample, or `unknown` before
the first), the number of records in `ATTEND.CSV`, the number of files in the
`LECTURES` folder, the last tap (card and time), the current lecture with the
device-clock time it started (`module / lecture (since ...)`, or `none set`),
and what the host's current `SETTINGS.CSV` would do when the session ends:
unchanged, will be applied (clock set, device ID change, all records deleted,
new lecture), or an error, in which case nothing is applied. The PC uses the
lecture's start time, which is the device's own clock, to decide which taps
belong to which lecture whatever its own clock says.

The battery figure is `batt_percent()` (`battery.c`): a piecewise-linear Li-ion
discharge curve at light load, 0 % at `APP_BATT_CUTOFF_MV` (where the unit
switches itself off) and 100 % at 4.2 V. A cell on charge reads high, and so
does the figure, which on `STATUS.TXT` is the usual case, since the cable is in.
The main loop passes each sample to `usbs_set_battery()`; it is also
`dbg_battery_percent`.

### Editing the settings over USB

`SETTINGS.CSV` as first shown:

```
# Edit these lines, then eject the drive (or press the button). Add #NEWSESSION,1 to start another lecture with the same names.
#TIME,2026-10-06 14:30:00
#MODULE,EN2090
#LECTURE,Circuits Lecture 4
#DEVICE,0000012345
```

The `#DEVICE` line appears once an ID is set. The device shows its current
settings back, so a host that edits only the lines it cares about leaves the
rest as they were. Nothing reaches the device while the drive is
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
and plugged in again, or on a double press (`BTN_DOUBLE`: two taps whose
releases are within `APP_BTN_DOUBLE_MS`, 600 ms), which clears `usb_hold_off`
and runs `usb_attach()`. The PC cannot bring it back itself: after the eject
the device has left the bus. The app's Connect button says to double-press.
The first tap of the pair is still an ordinary `BTN_SHORT` (the battery
display, or the end of a USB session). While VBUS is present the 3-minute idle switch-off is
suspended: Standby would bring the drive back on the next wake instead of the
reader. `usb_leave()` stops the USB peripheral and calls `usbs_end()`, which finds the file by its exact 8.3 name `SETTINGS.CSV` (a file
saved under any other name is ignored), follows its cluster chain through the
window, and parses it with `setf_scan()`, which never touches flash. Only after
the whole file is accepted is anything applied.

Rules for the file (`settings_file.h` has the full list): CRLF, LF or CR line
ends; UTF-8 BOM skipped; only lines whose first non-blank character is `#` mean
anything and every other line is ignored, so an old file that still lists
students does no harm; a `#` line with an unknown keyword is a comment, which
includes `#CARDS` from an older app (its numbers are ordinary lines, so they are
ignored too); a comma or quote inside a name becomes a space and quoted values
work. A file that is empty, larger than the window, or whose cluster chain
is broken is refused.

| Directive | Effect |
|---|---|
| `#TIME,YYYY-MM-DD HH:MM[:SS]` | sets the RTC, but only when it differs from the value the file was shown with, so an untouched file never winds the clock back to the moment of attach |
| `#DEVICE,<id>` | sets the device ID (decimal or `0x` hex), stored in the config page and used as the volume serial |
| `#MODULE,<name>` / `#LECTURE,<name>` | name the lecture (24 and 32 bytes). A new session starts when either differs from what the file was shown with. A name the file omits keeps its current value, and clearing both ends the lecture |
| `#NEWSESSION` | starts a new session even if the names are unchanged (a second lecture with the same names) |
| `#CLEARLOG` | erases the whole log (`log_erase_all()`), the RAM buffer and the 10 s table, before any new session marker is written. The Companion app sends it with a lecture start and with *Clear device records*, in both cases only after it has read every row of `ATTEND.CSV` and `LECTURES.CSV` into its database and the row count matched `STATUS.TXT`; *Erase device without saving…* sends it without that import, behind a typed confirmation. Renaming the lecture by hand never clears anything |

The session marker is written only if the import succeeded. A malformed `#TIME`
or `#DEVICE` value is reported on `STATUS.TXT` and ignored; it does not fail the
import.

When the cable comes out (`usb_leave()` in `app_fsm.c`, after the USB peripheral
has stopped) the unit applies the edit, shows the result and carries on
scanning: green and two pulses for applied (`FB_SAVED`), red and three pulses
for refused (`FB_REJECTED`). If the host changed nothing there is no pattern.
A new device ID is read back from flash before success is claimed; a mismatch
shows red.

**Power loss during the apply.** The only flash write besides the log is the
config page (device ID). It is erased and then programmed with the magic last,
so an interruption leaves "no configuration" (device ID 0) until the host sets
the ID again, never a page that half reads. The marker is appended to the log
like any other record.

### Registering a card while plugged in

The reader stays on during a USB session (`usb_attach()` leaves it
enabled), but a card read then is not attendance. `run_usb()` runs the reader just as
`run_idle()` does, wake-up mode included, and hands a card to `enrol_card()`
instead of `handle_card()`: nothing is logged and no duplicate check applies.
`usbs_set_last_card()` stores the card ID and UID (inside a critical section,
since the USB interrupt reads them) and counts the tap, the unit shows green and
one buzz (`FB_ACCEPTED`), and the scan counts in `scans_enrolled`
(`APP_SCAN_ENROLLED` in `dbg_scan_result`). `LASTCARD.TXT` is rendered afresh
at every read (`lastcard_sector()`):

```
LAST CARD
Taps    : 2
Card ID : 0183769758
UID     : 0A F4 1A 9E
```

`Taps` counts the cards read since this USB session began, from 0 at each
attach, so the PC notices the same card tapped twice; before the first tap the
ID reads `none yet` and the UID `-`. A host caches files, so the PC app reads
this one past its cache (as it does `STATUS.TXT`) and fills in the card when
`Taps` goes up: **Tap a card…** on the Students page, or **Tap card on device…**
in the student dialog. Once the drive is ejected or a button press ends the
session, taps are attendance again. A card can also be registered afterwards:
the app lists cards in the log that belong to nobody and offers to register
each one.

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

### Card detection: wake-up mode

With `APP_NFC_USE_WAKEUP` set, the reader is not polled while the field is
empty. `card_reader.c` alternates between two states:

```
armed ──EXTI1 / APP_EVT_NFC_WAKE──> polling every 100 ms
  ^                                     │
  └── measure the empty field, arm <────┘ after 3 empty polls in a row
```

- **Arming** (`plat_nfc_wakeup_arm()`): Level 2 measures the antenna
  amplitude with the field empty and passes it as the reference. The BSP
  writes it to the amplitude reference register, sets the wake-up timer to
  `APP_NFC_WAKE_PERIOD_MS` (100 ms) with amplitude measurement on, unmasks
  only I_wam, enters wake-up mode (`en` = 0, `wu` = 1) and unmasks EXTI1. The
  chip then drives the antenna briefly every period on its own RC timer and
  interrupts when a reading differs from the reference by more than the
  trigger window (`delta`, passed in by Level 2). **Every other chip interrupt
  must be masked in wake-up mode** (`irq_masks(true)`): the chip starts its
  oscillator for each measurement, so an unmasked I_osc raised the pin every
  100 ms. On the bench that showed as 99 % false wake-ups with the wake-up
  reading equal to the reference; `dbg_nfc_wake_irq` now records what raised
  each wake-up (0x040000, I_wam, is the only right answer). Leaving wake-up
  mode restores the normal masks before the oscillator is waited on.
- **The reference and the window are learned.** The reference is a Measure
  amplitude reading, but the chip compares its own wake-up measurement, which
  may sit a few counts away. `plat_nfc_wakeup_disarm()` hands up that
  measurement (register 0x36); after a wake-up that found no card,
  `card_reader.c` keeps the difference as `offset` and adds it to every later
  reference. False wake-ups that continue after that are noise: from the
  second in a row the window widens by one count, from
  `APP_NFC_WAKE_DELTA_MIN` (2) up to `APP_NFC_WAKE_DELTA_MAX` (10), and it
  narrows by one after `APP_NFC_WAKE_RELAX_AFTER` (20) wake-ups in a row that
  found a card. Both start afresh at every boot.
- **Waking**: the EXTI1 handler masks its line (one shot: the pin stays high
  until the status registers are read, and SPI belongs to the main loop) and
  posts `APP_EVT_NFC_WAKE`. The FSM calls `cr_wake()`, which leaves wake-up
  mode (`plat_nfc_wakeup_disarm()` reads the status, which drops the pin, and
  waits for `osc_ok`) and polls at once.
- **Polling** then runs exactly as without wake-up mode. A held card keeps the
  reader polling, so it is still reported once, and its removal is still seen.
  After `APP_NFC_REMOVE_MISSES` empty polls the reader arms again.
- **Arming only ever follows an empty poll.** A card measured into the
  reference would never wake the reader. So after any pause (a feedback
  pattern, a supply-mode change) `cr_enable()` polls once first, and arms
  only if that finds nothing. The reader runs the same way during a USB
  session, for registering cards.
- **A fresh reference each time** keeps drift from building up. If the cell
  voltage or the surroundings move the amplitude past the delta, the reader
  wakes, finds nothing, and re-arms against the new level. Such wake-ups are
  counted in `dbg_nfc_false_wakes`.
- **No commands in wake-up mode.** The oscillator is off there, so the regulator
  adjustment and the amplitude measurement need Ready mode. `sample_battery()`
  disables the reader before changing the supply mode, and `run_idle()` skips
  its 10 s amplitude reading while armed (`dbg_nfc_amplitude` then shows the
  reference). The host stubs count any command sent in wake-up mode
  (`host_nfc_misuse`), and the tests require none.

**Tuning on the bench.** Watch `dbg_nfc_armed`, `dbg_nfc_wakeups`,
`dbg_nfc_false_wakes`, `dbg_nfc_wake_irq`, `dbg_nfc_wake_raw`,
`dbg_nfc_wake_offset`, `dbg_nfc_wake_delta` and `dbg_nfc_amplitude`. A
false wake-up or two after power-on is the offset being learned. A
`dbg_nfc_wake_irq` other than 0x040000 is a masking bug. A window stuck at
`APP_NFC_WAKE_DELTA_MAX` with false wake-ups still climbing is noise or an
object near the antenna; cards that are read only very close mean the window
grew too large for this antenna. Setting `APP_NFC_USE_WAKEUP` to 0 brings back
plain 100 ms polling for comparison.

**Reading the globals with a hot-plugged probe.** CubeIDE's debug session sets
C_DEBUGEN, and `plat_sleep_until()` then never sleeps, so Live Expressions are
reliable. A probe attached without halting debug (STM32CubeProgrammer
`mode=HOTPLUG`) reads zeros for most words while the core sleeps, flash
included. Halt for the read: `STM32_Programmer_CLI -c port=SWD mode=HOTPLUG
shared -halt -u <addr> <len> out.bin -run` (a millisecond, which USB does not
notice). No build keeps the debug clocks on in Sleep, Stop 2 or Standby:
`bsp_power_init()` clears `DBGMCU_CR` (DBG_SLEEP, DBG_STOP, DBG_STANDBY) at
every boot, and Stop 2 and Standby entry clear it again. The bits survive every
reset but a power-on one, and with them set the unit drew 0.3 mA switched off.
So a probe cannot attach while the core is in Stop 2: attach while it is awake
(plug its USB into a PC, which keeps the loop in Sleep mode),
or hold BOOT0 (SW2) and press reset (SW1) to enter the bootloader.

**Flashing over SWD toggles FLASH_SR.PEMPTY ("main flash is empty").**
Measured on the bench, with no reset in between: 0 before programming, 1
after, 0 after a second pass. ST's flash loader (STM32CubeProgrammer, and
CubeIDE through the ST-LINK GDB server) clears status flags with a mask that
includes the bit, and on this part writing 1 to it flips it. With it set,
every reset that is not a power-on (the tool's, the reset button, the
debugger's) boots the system bootloader (USB ID 0483:df11, DFU), because the
chip re-reads the flag only at power-on or option-byte reload; `RCC_CSR`
showed only a software reset, no brown-out and no option reload. Hence "it
sometimes does not run after flashing": it depends on the flag's state before
the flash.

The firmware now deals with it whenever it gets to run once:
`bsp_early_init()`, the first call in `main()`, sets `SCB->VTOR` to
`FLASH_BASE` (with the flag set, address 0 maps the bootloader, so a program
a debugger starts after a reset would otherwise take its interrupts from the
bootloader's vectors) and flips a set PEMPTY back (`dbg_pempty_cleared` = 1).
That covers CubeIDE debug sessions, which halt at the reset vector and start
the program themselves. It also stops the HAL taking the flag for a flash
error, which used to fail the first log write after a flash. A flash followed
straight by a plain reset (CubeProgrammer `-rst`, CubeIDE Run) never runs the
program, so if the board then shows up as DFU: power-cycle it (USB and
battery), or flash once more (the flag flips back), or toggle FLASH_SR bit 17
(write 0x00020000 to 0x40022010) and reset.

## Flow-chart coverage

| Flow chart | Where |
|---|---|
| Start / battery OK? / load config | `app_init()` |
| card present → power RF → read | the reader's wake-up (`APP_EVT_NFC_WAKE`), `run_idle()`, `card_reader.c`, `iso14443a.c`, `bsp_nfc.c` |
| valid ID? (BCC, CRC_A, cascade) | `iso14443a_select()` |
| same ID within 10 s? / already signed in? | `dedup.c` (8-entry MRU table) and `sess_card_seen()`, see "Duplicate taps" |
| known card? | not on the device: every new card is logged with `FB_ACCEPTED` (green); the PC app decides who is registered |
| create record, RAM buffer | `handle_card()`, `record_buffer.c` |
| RAM buffer ≥ 80 % | `rb_needs_flush()` → `log_flush()` |
| 3-min inactivity → flush → Standby | `APP_EVT_INACTIVITY` → `begin_shutdown()` |
| USB attach → enumerate → CSV | `usb_attach()`, `usb_storage.c`, `bsp_usb.c` |
| card read during USB (registering) | `run_usb()` → `enrol_card()` → `usbs_set_last_card()`, `LASTCARD.TXT`; not logged |
| low battery → flush → Standby | `sample_battery()` → `APP_EVT_LOW_BATTERY` |

Branches the flow chart does not have:

- **Every card is recorded.** The device has no student list to check, so
  "ID in student list?" always takes the yes branch.
- **A card read on USB is not attendance.** It is shown to the PC for
  registering (`LASTCARD.TXT`) and not logged.
- **A charger is not a host.** VBUS that does not enumerate within 5 s is
  treated as a charger: USB is stopped again and taps are logged from then on.
  Until a host has enumerated, the reader waits (`run_usb()` enables it only
  with `usb_host` set), so a card held through those seconds is logged when the
  session ends rather than shown in a `LASTCARD.TXT` nobody reads.
- **Settings when the USB session ends.** On an eject, a button press or the
  cable coming out, an edited `SETTINGS.CSV` is applied (green, two pulses) or
  refused (red, three pulses), and scanning carries on, with the cable still in
  after an eject or a press.

`handle_card()` checks in this order: the 10 s table (`dedup.c`), then "already
recorded in this lecture" (`sess_card_seen()`), then whether the log has room,
and only then records the card (green).

Duplicate suppression uses an eight-entry MRU table rather than the single
last-seen slot the diagram implies. With one slot, two people tapping in
alternation each clear the other's entry and both get logged twice; there is a
test for that case.

## User interface

| Event | LEDs | Motor |
|---|---|---|
| Power on | green 300 ms | 120 ms |
| Card accepted | green 250 ms | 90 ms |
| Card read during a USB session (registering, not recorded) | green 250 ms | 90 ms |
| Duplicate (within 10 s, or already in this lecture) | green ×2 | ×2 short |
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
keeps them) and declared in `App/Inc/app_debug.h`: battery millivolts, raw
counts and percentage (`dbg_battery_percent`), the last card's UID, ATQA, SAK and logged ID, the scan result, button
state and press counts, reader status, counters and interrupt flags, the
wake-up state and counts (`dbg_nfc_armed`, `dbg_nfc_wakeups`,
`dbg_nfc_false_wakes`), how long the last pass asked to sleep
(`dbg_sleep_ms`), record counts, and the RTC. The host build defines its own
copies in `Tests/host_platform.c`.

To set the clock from the debugger, fill in `dbg_set_time` and set
`dbg_set_time_request` to 1. A unit whose RTC was never set starts from the
firmware's build time.

## Companion app

`Companion/` holds the app for the computer the device is plugged into: a
desktop window built with Qt (`attendance_qt.py`, PySide6; it says how to
install PySide6 and exits when it is missing). There is no other window, no
browser version and no server. The window's non-GUI helpers are in
`app_helpers.py`; the actions and device access in `attendance_app.py`. It recognises the device by `ATTEND.CSV`, `SETTINGS.CSV`
and a `STATUS.TXT` that begins `ATTENDANCE LOGGER`, reads `ATTEND.CSV`,
`LECTURES.CSV`, `STATUS.TXT` and `LASTCARD.TXT` (the last two past the OS file
cache, since the device regenerates them at every read), and rewrites only
`SETTINGS.CSV`, in place. It does not use the `LECTURES` folder.

Everything the device does not know lives in a SQLite database
(`attendance_db.py`, `~/AttendanceLogger/attendance.db`, with a dated backup once
a day): students (card, name, student number, department, modules), modules,
lectures and every tap. Reading the device adds its taps (known ones are
skipped); attendance is worked out by matching taps to lecture time windows,
using the lecture start times the device lists in `LECTURES.CSV`. Which taps
are registered students is decided there too: the app sends the device no card
list. It registers cards over USB through `LASTCARD.TXT` (see above), shows the
battery from `STATUS.TXT` while the device is connected, and clears the device
with `#CLEARLOG` only after importing everything, except for *Erase device
without saving…*, which skips the import behind a typed `ERASE`. Reports come
out as CSV or as PDF (`report_pages.py` makes a page that headless Chrome or Edge
prints; with neither, Print opens the page in the browser, then Save as PDF).
`Companion/README.md` describes use.

## Board notes for the hardware

- **PC14/PC15 need a 32.768 kHz crystal.** Not optional: the RTC runs from
  it, and so does LPTIM1, which times and ends every Stop 2 sleep.
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
- **PA0 is the power button**, active low to ground; it is WKUP1, and while
  running its edges (EXTI0) wake the loop, which samples the level.
- USB is crystal-less: HSI48 trimmed by the CRS against the host's SOF.
- **The MCU is the 128 kB STM32L432KB.** The flash-size word at `0x1FFF75E0`
  reads 128 on the bench unit. A 256 kB KC would run this firmware unchanged,
  using only the bottom 128 kB.

Full pin map: `Bsp/Inc/bsp_board.h`.

## Known limitations

Most were found in review on 2026-10-06; none is covered by a test yet. (A long press
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
  that is cut short at the 768th in `ATTEND.CSV`, `LECTURES.CSV` and the
  `LECTURES` folder, silently.
  With the Companion app's `#CLEARLOG` at every lecture start the log holds the
  markers since the last one, a few lectures started from the button at most, so
  this matters only to a device used without the app.
- **Changing the device ID erases the config page first.** A power cut between
  the erase and the final write leaves no device ID (0, and the default volume
  serial) until it is set again.
- **Host editors.** The root directory has 16 entries, seven of them the
  device's own, and a long file name takes extra entries; macOS metadata files
  use root entries too, so a save can fail with "disk full" for want of an
  entry. `SETTINGS.CSV` is a few hundred bytes now, so the 28-cluster window
  leaves an editor plenty of room for a temporary copy. Excel may also rewrite `#TIME` in a locale format the parser does not
  accept. Edit `SETTINGS.CSV` with a plain text editor, or let the Companion app
  write it.
- **Settings apply when the session ends.** `#TIME` is applied at the eject,
  button press or unplug, so the clock is late by however long the drive stayed
  mounted after the file was written (the app ejects straight away).
- **A tap in the first seconds on a charger is late.** Until VBUS has gone
  `APP_USB_ENUM_TIMEOUT_MS` (5 s) without enumerating, the reader waits for a
  host. A card held through that is read and logged when the wait ends; one
  taken away sooner gets no feedback and has to be tapped again.
- **Eject detection relies on START STOP UNIT.** A host that ejects some other
  way (or only suspends the port) leaves the drive session running; the button
  or the cable still end it. A Windows or Linux host that sends START = 0 for
  disk power management would end the session as well.

## Provisioning

Nothing needs provisioning for the device to log: it records every card it
reads, and keeps no card list. Until the RTC is set it starts from the
firmware's build time; set the clock once (the app does it when a lecture is
started, or use `#TIME` in `SETTINGS.CSV`). The device ID is optional and set
with `#DEVICE`. Students are entered in the PC app, which never sends them to
the device. `Tests/fs_check.sh` drives the whole USB path with real FAT tools.

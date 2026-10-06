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

`Tests/` compiles every Level 2 module against a RAM-backed stub platform with
a simulated ISO14443-A card, and runs 150-odd assertions in under a second:
card activation with 4-, 7- and 10-byte UIDs, presence tracking, button
debouncing, exhaustive calendar round trips from 2000 to 2099, the flash log's
recovery from a power loss mid-page, the FAT12 image a host has to accept, and
the whole state machine driven end to end on simulated time. None of that
needs hardware, a debugger, or a card.

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
                      log_store, student_db, record_buffer, dedup, feedback,
                      battery, csv, fat12, usb_storage, timeutil, crc,
                      app_events, app_debug
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
| Standby | 3 min idle, low battery, long press | RTC + backup registers | WKUP1 (button) only, through reset |

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
struct { uint32_t student_id; uint32_t stamp; };   /* epoch 2000-01-01 */
```

Exactly one STM32L4 flash double-word, the smallest programmable unit. Record
size and programming granularity being identical removes read-modify-write
from the log entirely: a power loss can only lose the record being written,
never corrupt one already stored.

### Flash map — top 128 kB (`0x08020000`)

```
page  0        config + student list descriptor
pages 1..8     student list, 4096 sorted 32-bit IDs, searched in place
pages 9..63    attendance log, 55 pages x 254 records = 13 970 records
```

The linker script's `FLASH` region was shortened to 128 kB and an `NVDATA`
region added, so an image that would overlap the log fails to link instead of
erasing records at run time.

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

### CSV — 32 bytes per row, header included

```
SCAN_DATE,SCAN_TIME,STUDENT_ID<CR><LF>
2026-09-10,13:27:45,0000123456<CR><LF>
```

Fixed width, and 32 divides 512 exactly: sixteen rows per sector, no row ever
straddling a sector boundary. A mass-storage host reads sectors in whatever
order it likes, and this turns "which records are in sector N?" into a
multiply — no scan, no RAM-resident index.

### USB volume

A 1 MB read-only FAT12 volume holding one file, `ATTEND.CSV`, synthesised
sector by sector. Nothing is stored: boot sector, both FATs, the root
directory and the file body are all computed on demand. Geometry gives 2034
clusters, safely under the 4085 above which a host would read the volume as
FAT16.

The record count is latched at attach. A host that saw the file size change
mid-copy would produce a truncated CSV, so scans arriving during a USB session
stay in RAM and appear on the next attach.

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
| ID in student list? | `student_allowed()` → `sdb_contains()` |
| same ID within 10 s? | `dedup.c` — an 8-entry MRU table, see below |
| create record, RAM buffer | `handle_card()`, `record_buffer.c` |
| RAM buffer ≥ 80 % | `rb_needs_flush()` → `log_flush()` |
| 3-min inactivity → flush → Standby | `APP_EVT_INACTIVITY` → `begin_shutdown()` |
| USB attach → enumerate → CSV | `usb_attach()`, `usb_storage.c`, `bsp_usb.c` |
| low battery → flush → Standby | `sample_battery()` → `APP_EVT_LOW_BATTERY` |

Two branches the flow chart does not have:

- **No student list.** With `APP_ACCEPT_ALL_WHEN_NO_LIST` set, an unprovisioned
  unit records every card instead of rejecting every card.
- **A charger is not a host.** VBUS that does not enumerate within 5 s is
  treated as a charger: USB is stopped again and scanning carries on.

Duplicate suppression uses an eight-entry MRU table rather than the single
last-seen slot the diagram implies. With one slot, two people tapping in
alternation each clear the other's entry and both get logged twice; there is a
test for that case. Unknown cards are checked before duplicates, so an
unknown card always gets the red pattern.

## User interface

| Event | LEDs | Motor |
|---|---|---|
| Power on | green 300 ms | 120 ms |
| Card accepted | green 250 ms | 90 ms |
| Duplicate (within 10 s) | green ×2 | ×2 short |
| Unknown card | red 450 ms | 450 ms |
| Log full, or reader failed at power-on | red ×4 | ×4 |
| Button tap | green ×2 (battery OK) or red ×5 (low) | — |
| Button held 2 s, or 3 min idle | red 700 ms, then off | 250 ms |
| Idle | 30 ms green flash every 4 s; red if the battery is low or the reader failed | — |
| USB session | green flash every second | — |

## Live debugging

`App/Inc/app_debug.h` declares `dbg_*` globals for the STM32CubeIDE Live
Expressions view: battery millivolts and raw counts, the last card's UID,
ATQA, SAK and logged ID, the scan result, button state and press counts,
reader status and counters, record counts, and the RTC. `bsp_nfc.c` adds
`dbg_nfc_last_irq` and `dbg_nfc_irq_pin_misses`.

To set the clock from the debugger, fill in `dbg_set_time` and set
`dbg_set_time_request` to 1. A unit whose RTC was never set starts from the
firmware's build time.

## Board notes for the hardware

- **PC14/PC15 need a 32.768 kHz crystal.** Not optional: the RTC runs from
  it, and the move to interrupts puts both LPTIMs on it too.
- **PB7 (`PVD_IN`) must be tied to the battery divider node**, the same node
  as PA7. The PVD's levels 0–6 watch VDD, which a regulator holds steady until
  it drops out entirely — by which point it is too late to write flash. Level 7
  compares PVD_IN against VREFINT and so actually tracks the cell.
- **The divider is 4.7 M over 2.7 M with 100 nF across the low leg.** It is
  permanently connected because the PVD watches it, so it is sized for ~0.5 µA;
  the cap keeps the source impedance low enough for the ADC's 640.5-cycle
  sampling window.
- **PA0 is the power button**, active low to ground; it is WKUP1, and is
  polled while running.
- USB is crystal-less: HSI48 trimmed by the CRS against the host's SOF.

Full pin map: `Bsp/Inc/bsp_board.h`.

## Provisioning

The student list and config page are not written by the firmware. Page 0 takes
an `nv_config_t` (magic `"CAS1"`, count, CRC-32, device ID) and pages 1–8 take
the sorted IDs. `Tests/test_main.c:provision_students()` shows the exact
layout. A corrupt CRC makes the firmware refuse the list entirely — every card
then reads as unknown, which is visible, rather than silently accepting a list
that may have lost entries.

Until the list is provisioned every card is recorded
(`APP_ACCEPT_ALL_WHEN_NO_LIST`). Until the RTC is set it starts from the
firmware's build time.

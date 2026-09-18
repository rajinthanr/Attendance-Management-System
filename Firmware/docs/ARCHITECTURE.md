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
94 assertions in under a second: EM4100 round trips at two data rates and with
jitter, exhaustive calendar round trips from 2000 to 2099, the flash log's
recovery from a power loss mid-page, and the FAT12 image a host has to accept.
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
App/Inc, App/Src      Level 2. app_fsm, em4100, log_store, student_db,
                      record_buffer, dedup, feedback, battery, csv, fat12,
                      usb_storage, timeutil, crc, app_events
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
  stepped by LPTIM2, so a 450 ms "unknown card" buzz costs one wake per step
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
physical position. An opened-but-unsealed page is unambiguously "power went
away mid-write", and `log_init()` adopts it and continues appending — there
is a test for exactly that.

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
| same ID within 10 s? | `dedup.c` — an 8-entry MRU table, see below |
| ID in student list? | `sdb_contains()` |
| create record, RAM buffer | `handle_tag()`, `record_buffer.c` |
| RAM buffer ≥ 80 % | `rb_needs_flush()` → `log_flush()` |
| 3-min inactivity → flush → Standby | `APP_EVT_INACTIVITY` → `shutdown()` |
| USB attach → enumerate → CSV | `usb_attach()`, `usb_storage.c`, `bsp_usb.c` |
| PVD low battery → flush → Standby | `APP_EVT_LOW_BATTERY` |

Duplicate suppression uses an eight-entry MRU table rather than the single
last-seen slot the diagram implies. With one slot, two people tapping in
alternation each clear the other's entry and both get logged twice; there is a
test for that case.

## Board notes for the hardware

- **PC14/PC15 need a 32.768 kHz crystal.** Not optional: the RTC and both
  LPTIMs depend on it, and without it nothing survives Stop 2.
- **PB7 (`PVD_IN`) must be tied to the battery divider node**, the same node
  as PA7. The PVD's levels 0–6 watch VDD, which a regulator holds steady until
  it drops out entirely — by which point it is too late to write flash. Level 7
  compares PVD_IN against VREFINT and so actually tracks the cell.
- **The divider is 4.7 M / 4.7 M with 100 nF across the low leg.** It is
  permanently connected because the PVD watches it, so it is sized for ~0.4 µA;
  the cap keeps the source impedance low enough for the ADC's 640.5-cycle
  sampling window.
- **PA0 is the power button**, active low to ground; it is both WKUP1 and EXTI0.
- USB is crystal-less: HSI48 trimmed by the CRS against the host's SOF.

Full pin map: `Bsp/Inc/bsp_board.h`.

## Provisioning

The student list and config page are not written by the firmware. Page 0 takes
an `nv_config_t` (magic `"CAS1"`, count, CRC-32, device ID) and pages 1–8 take
the sorted IDs. `Tests/test_main.c:provision_students()` shows the exact
layout. A corrupt CRC makes the firmware refuse the list entirely — every card
then reads as unknown, which is visible, rather than silently accepting a list
that may have lost entries.

Until the list is provisioned every tag reads as unknown, and until a host sets
the RTC the calendar starts at 2026-01-01.

# Card Attendance System

A battery-powered NFC attendance logger. A student taps an ID card and the unit
confirms the tap with an LED and a vibration pulse, and stores the card number
with a timestamp in on-chip flash. Plugged into a PC over USB-C, it appears as a
USB drive, with no driver needed:

- `ATTEND.CSV`: every tap, read-only.
- `STATUS.TXT`: a short summary of the device.
- `SETTINGS.CSV`: edited by the PC to set the clock, start a lecture, and send
  the list of registered card numbers.

A desktop companion app for the PC keeps the student names and works out who
attended which lecture.

The repository holds the hardware (KiCad 10), the firmware (STM32CubeIDE /
Makefile) and the companion app (Python).

## Project status (2026-10-06)

| Area | State |
|---|---|
| Schematic and PCB | Complete and sent to JLCPCB. DRC still reports two missing connections on the antenna loop (AE1). |
| Firmware | Runs on the board in polling mode. Card reading, LEDs, vibration and the USB drive work. Battery measurement reads full scale on the bench (see [Known issues](#known-issues)). 829 host checks pass. |
| Companion app | Desktop app (Tkinter); the earlier browser version is still included. 123 tests pass, including cross-checks against the firmware's own parser. |

## How it works

1. **Card tap.** The ST25R3916 reader polls for ISO14443-A cards (MIFARE
   Classic, NTAG and similar) every 100 ms. The card number is its UID: 4-byte
   UIDs as they are, longer ones by their last four bytes.
2. **Check.** A card read again within 10 s, or already recorded in the current
   lecture, is a duplicate and is not recorded again. Every other card is
   recorded. The device's card list only decides the colour: green for a
   registered card, red for one it doesn't know.
3. **Feedback.**

   | Result | Feedback |
   |---|---|
   | Registered card | green LED and one short buzz |
   | Unregistered card (still recorded) | red LED and one long buzz |
   | Duplicate | two short buzzes |
   | Log full, or reader fault | red blinks with buzzes |
   | Settings applied / refused (eject, button tap or unplug) | green and two buzzes / red and three buzzes |

4. **Logging.** Taps go to a RAM buffer, then to flash within 5 seconds. The log
   holds 6,858 records and survives a power cut. When it is full, new taps are
   refused rather than overwriting old ones.
5. **Lectures.** Starting a lecture (module and lecture name) writes a marker
   into the log, so taps are grouped by lecture even without the app. A card
   counts once per lecture. The app imports every tap before a lecture starts
   and has the device delete its copy (`#CLEARLOG`).
6. **USB.** Plug in USB-C. The reader stops and the drive appears. Ejecting
   the drive or tapping the button applies `SETTINGS.CSV` and starts the reader
   with the cable still in (unplugging does the same). A USB charger that is not
   a computer is recognised after 5 s, and scanning carries on while it charges.
7. **Power.** Press the power button to switch on. A tap shows the battery
   level; holding it for 2 s switches the device off. On battery it also switches
   itself off after 3 minutes without a tap, or when the battery is flat.

`Firmware/docs/USER_GUIDE.md` explains the device for the people who run a
class with it.

## Hardware

| Function | Part | Notes |
|---|---|---|
| MCU | STM32L432**KB** (Cortex-M4F, 128 kB flash, 64 kB RAM, QFN-32) | Runs at 4 MHz from MSI, and 24 MHz during USB sessions, with a 32.768 kHz LSE crystal. The 128 kB of flash holds both the firmware (72 kB) and the attendance log (56 kB). |
| NFC reader | ST25R3916 (13.56 MHz, QFN-32) on SPI1 | Differential antenna drive with an EMC filter, a matching network and a capacitive RX divider; 27.12 MHz crystal. |
| Antenna | PCB loop, `NFC_Loop_40x30_3T` (40 × 30 mm, 3 turns) | In the keep-out area at the top of the board. |
| Backup reader | 8-pin header for a PN532 breakout (Elechouse V3 SPI pinout) | Shares SPI1, with its own chip-select (PA15) and IRQ (PB6). Not used by the firmware yet. |
| Charger | MCP73833 Li-ion linear charger | 10 kΩ NTC and three charge-status LEDs. |
| 3.3 V rail | TPS7A0233 LDO, 200 mA | Runs from the cell. The reader's VDD/VDD_TX come straight from the battery, and its VDD_IO from 3.3 V. |
| USB | USB-C receptacle (GCT USB4110), USB 2.0 full speed | Sink-only with 5.1 kΩ CC resistors; USBLC6-2SC6 ESD protection. |
| Battery | Single-cell Li-ion, 3.7 V, 2-pin JST-XA | Sensed through a 4.7 MΩ / 2.7 MΩ divider with 100 nF, on the ADC (PA7) and the PVD input (PB7). |
| Feedback | Red and green LEDs, vibration motor | The motor is switched by an AO3400A MOSFET with an SS14 flyback diode. |
| Buttons | Power (PA0 / WKUP1), reset, BOOT0 | |
| Debug | 4-pin SWD header: 3V3, SWDIO, SWCLK, GND | |

### MCU pin map

| Pin | Signal | Pin | Signal |
|---|---|---|---|
| PA0 | Power button (WKUP1, polled) | PB0 | ST25R3916 chip-select |
| PA2 / PA3 | Green / red LED | PB1 | ST25R3916 IRQ (polled) |
| PA4 | Vibration motor enable | PB3 / PB4 / PB5 | SPI1 SCK / MISO / MOSI |
| PA7 | Battery sense (ADC1_IN12) | PB6 | PN532 IRQ |
| PA9 | VBUS detect (polled) | PB7 | Battery sense (external PVD input) |
| PA11 / PA12 | USB D- / D+ | PA15 | PN532 chip-select |
| PA13 / PA14 | SWDIO / SWCLK | PC14 / PC15 | 32.768 kHz crystal |

`Firmware/Bsp/Inc/bsp_board.h` is the firmware's reference for the pin map, and
`Firmware/Card Attendance System.ioc` for the CubeMX configuration.

### PCB

- **Board:** 60 × 88.6 mm, 2 layers, 1.6 mm FR-4, all parts on the top side.
- **Floorplan:** the NFC antenna fills the top of the board. Below it are the
  PN532 header, the MCU and the ST25R3916 front end. The power section (USB-C,
  ESD, charger, LDO, battery connector) runs along the bottom edge.
- **Schematic sheets:**
  - `power.kicad_sch`: USB-C, ESD, charger, LDO and battery.
  - `mcu.kicad_sch`: STM32, buttons, LEDs, motor driver, SWD and the PN532
    header.
  - `rfid.kicad_sch`: ST25R3916, crystal, EMC filter, matching network and
    antenna.
- **Rules:** manufacturing rules are in `PCB/Attendance Management System.kicad_dru`.
  The production files sent to JLCPCB are in `PCB/production/`.

## Firmware

The firmware is split along one rule: **the application logic (`App/`) decides,
the drivers (`Bsp/`) act.**

- **`App/`:** the state machine, ISO14443-A protocol, card presence tracking,
  button debouncing, flash log, card list and device config, lecture sessions,
  the `SETTINGS.CSV` parser, the FAT12 volume, CSV and status files, and battery
  maths. It includes no HAL or register headers and compiles on a PC.
- **`Bsp/`:** implements the hardware contract in `App/Inc/platform_if.h`,
  handing up only raw values. It includes the register-level ST25R3916 driver
  (`bsp_nfc.c`).
- **`Tests/`:** builds all of `App/` against a RAM-backed stub platform with a
  simulated card and a simulated PC editing the drive, so the logic is tested
  without hardware.

The firmware currently **polls**: the main loop runs once a millisecond, and only
SysTick and USB use interrupts. `Firmware/docs/ARCHITECTURE.md` describes the
whole design: polling and the plan for interrupts, the flash layout, record, CSV
and settings formats, sessions, the USB volume, and known limitations.

For debugging, `dbg_*` globals defined in `Core/Src/main.c` show the battery
voltage, the last card, button presses, reader status and record counts in
STM32CubeIDE's Live Expressions view.

### Build and test

Run these from `Firmware/`:

```sh
make test    # host tests for the application logic; needs only a native C compiler
make         # cross-compiles build/card-attendance.{elf,hex,bin}
make clean
```

- **Toolchain:** `make` uses the GCC bundled with STM32CubeIDE
  (`/opt/st/stm32cubeide_*`) when one is installed, and otherwise
  `arm-none-eabi-gcc` on `PATH`.
- **Flashing and debugging:** use STM32CubeIDE over the SWD header.
- **Optional:** `Tests/fs_check.sh` checks the USB volume with real FAT tools
  (needs `mtools` and `dosfstools`).

### Data on the device

- **Flash layout:** the firmware takes the first 72 kB of the 128 kB flash; the
  last 56 kB (from `0x08012000`) is reserved for data by the linker script.
  - Its first page: configuration (device ID).
  - The other 27 pages: the attendance log, 6,858 records.
- **Records:** each record is 8 bytes (card number and seconds since
  2000-01-01), exactly one flash double-word. Lecture markers are records with
  reserved IDs from `0xFFFFFF00` up.
- **Setting up a device:** nothing is needed for it to log. The clock starts from
  the firmware's build time until it is set with `#TIME` in `SETTINGS.CSV` (the
  companion app does this when you start a lecture). The card list and device ID
  also come from `SETTINGS.CSV`.

## Companion app

`Companion/` is a desktop app for the PC the device is plugged into. It needs
only Python 3.8 or newer, with no install and no internet, and runs entirely on
that computer. It finds the device, reads its taps into a local SQLite database,
keeps student names, modules and lectures, starts lectures, sends the card list,
and produces attendance reports as CSV or PDF. Start it with `start.bat` on
Windows or `sh start.sh` on macOS and Linux. `Companion/README.md` explains it
in full.

## Repository layout

```
Firmware/            STM32CubeIDE project + standalone Makefile
  App/               application logic (portable, host-tested)
  Bsp/               board support: HAL drivers, ST25R3916 driver, USB MSC glue
  Core/              CubeMX-generated startup code (edit only inside USER CODE blocks)
  Drivers/           ST HAL and CMSIS
  Middlewares/       ST USB Device Library (core + MSC)
  Tests/             host test harness and tools
  docs/              ARCHITECTURE.md (design), USER_GUIDE.md (for users)
  Card Attendance System.ioc   CubeMX pin and peripheral configuration
Companion/           PC app (Python): desktop GUI, database, reports, tests
PCB/                 KiCad 10 project, BOMs (bom/) and JLCPCB production files (production/)
Enclosure/           3D-printable enclosure (enclosure.py and STL files)
CLAUDE.md            detailed engineering notes (design decisions, open issues)
LICENSE              MIT
```

## Tools

- **PCB:** KiCad 10.0.
- **Firmware:** STM32CubeIDE 1.19 (CubeMX 6.15, STM32Cube FW_L4 V1.18.2), or
  `arm-none-eabi-gcc` with `make`.
- **Host tests:** any C11 compiler (`cc`).
- **Companion app:** Python 3.8+ with Tkinter (included with the standard Python
  installers).

## Known issues

- **Battery reading at full scale.** On the bench the battery-sense pin reads
  full scale (`dbg_battery_error` = 4). The schematic, the layout and the BOM
  values are correct, and the firmware path has been checked. The next step is
  to measure the voltage across the 2.7 MΩ resistor on the running board. Until
  this is fixed, low-battery shutdown cannot work.
- **Firmware limitations.** `Firmware/docs/ARCHITECTURE.md` lists them under
  "Known limitations". The most important:
  - Start each lecture from the PC. Without a new lecture, a card already
    recorded in the last 6 hours counts as a duplicate.
- **Antenna tuning.** Tune the matching network and the RX capacitive divider
  against the real coil (ST AN5276). The reader measured a weak RX signal at
  first; the target amplitude reading is about 190.
- **Charge current.** The PROG resistor is 1 kΩ, which sets 1 A. That's more than
  the 500 mA a USB-C sink with plain 5.1 kΩ CC resistors may draw, and more than
  the MSOP-10 charger can dissipate. Use at least 2 kΩ.
- **External footprint library.** Some footprints come from a SnapEDA library at
  an absolute path outside the repository, so a fresh clone can't resolve them
  until the library is added or the paths are updated.
- **PN532 placement.** A PN532 module plugged into its header sits close to the
  ST25R3916 antenna and may detune it.

## License

MIT. See [LICENSE](LICENSE).

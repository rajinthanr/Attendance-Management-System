# Card Attendance System

A battery-powered NFC attendance logger. A student taps an ID card and the unit confirms the scan with an LED and a vibration pulse. It stores the scan in on-chip flash with a timestamp. Plugged into a PC over USB-C, it appears as a read-only USB drive holding one file, `ATTEND.CSV`, which needs no driver or app to read.

The repository holds the hardware (KiCad 10) and the firmware (STM32CubeIDE / Makefile) for the reader.

## Project status (2026-09-26)

| Area | State |
|---|---|
| Schematic | Complete. ERC: 0 errors, 1 warning (U5's embedded symbol differs from its library copy). |
| PCB layout | Placed and routed. The NFC loop antenna footprint (AE3) is still a net-tie placeholder on the board, so DRC reports the two antenna feed connections as open. Everything else is connected. |
| Firmware, application logic | Complete for 125 kHz EM4100 cards. 94 host tests pass. |
| Firmware, hardware drivers | Not ported to the new hardware yet. They still drive the original 125 kHz reader, not the ST25R3916. The cross-build currently fails because `Firmware/Middlewares/ST/STM32_USB_Device_Library` is missing (see [Known issues](#known-issues)). |

## How it works

1. **Card tap.** The ST25R3916 detects a card with its low-power capacitive wake-up (CSI/CSO electrode) and wakes the MCU.
2. **Read and check.**
   - The MCU reads the card ID over SPI.
   - It drops a repeat of the same card within 10 s.
   - It looks the ID up in a provisioned student list of up to 4096 IDs.
3. **Feedback.**

   | Result | Feedback |
   |---|---|
   | Accepted | Green LED and one short vibration |
   | Duplicate | Two short vibrations |
   | Unknown card | Red LED and one long vibration |
   | Low battery | Red LED blinks |

4. **Logging.**
   - Records (student ID and timestamp) are buffered in RAM, then written to flash in batches.
   - The flash log holds 13,970 records and survives power loss mid-write.
   - When the log is full, the unit refuses new scans rather than overwrite old ones.
5. **Export.** Plug in USB-C. The device enumerates as a 1 MB FAT12 drive containing `ATTEND.CSV`:

   ```
   SCAN_DATE,SCAN_TIME,STUDENT_ID
   2026-09-10,13:27:45,0000123456
   ```

6. **Power.**
   - The MCU stays in Stop 2 between scans.
   - After 3 minutes idle, a low battery (PVD) or a press of the power button, it flushes the log and enters Standby.
   - The power button (WKUP1) wakes it again.

## Hardware

| Function | Part | Notes |
|---|---|---|
| MCU | STM32L432KCU6 (Cortex-M4F, 256 kB flash, 64 kB RAM, QFN-32) | Runs at 4 MHz from MSI, and 24 MHz during USB sessions. Uses a 32.768 kHz LSE crystal. |
| NFC reader | ST25R3916 (13.56 MHz, QFN-32) on SPI1 | Differential antenna drive with an EMC filter, a matching network and a capacitive RX divider. Uses a 27.12 MHz crystal. |
| Backup reader | 8-pin header J7 for a PN532 breakout (Elechouse V3 SPI pinout) | Shares SPI1 with the ST25R3916, with its own chip-select (PA15) and IRQ (PB6). |
| Antenna | PCB loop, `NFC_Loop_40x30_3T` (40 × 30 mm, 3 turns) | Sits in the 57 × 37 mm keep-out area at the top of the board, fed through R21/R22 (0 Ω). |
| Charger | MCP73833 Li-ion linear charger | Charge current set by R6, with a 10 kΩ NTC (TH1) and three charge-status LEDs. |
| 3.3 V rail | TPS7A0233 LDO, 200 mA | Runs from the cell. The NFC reader's VDD/VDD_TX come straight from the battery, and its VDD_IO from 3.3 V. |
| USB | USB-C receptacle (GCT USB4110), USB 2.0 full speed | Sink-only with 5.1 kΩ CC resistors. USBLC6-2SC6 ESD protection on D+/D-/VBUS. |
| Battery | Single-cell Li-ion, 3.7 V, 2-pin JST-XA (J2) | Battery voltage is sensed by a divider (4.7 MΩ / 2.7 MΩ) on the ADC (PA7) and the PVD input (PB7), so the PVD trips at about 3.3 V. |
| Feedback | Red and green LEDs, vibration motor (J4) | The motor is switched by an AO3400A MOSFET with an SS14 flyback diode. |
| Buttons | Power (PA0 / WKUP1), reset, BOOT0 | |
| Debug | 4-pin SWD header (J3): 3V3, SWDIO, SWCLK, GND | |

### MCU pin map

| Pin | Signal | Pin | Signal |
|---|---|---|---|
| PA0 | Power button (EXTI0, WKUP1) | PB0 | ST25R3916 chip-select |
| PA2 / PA3 | Green / red LED | PB1 | ST25R3916 IRQ (rising edge) |
| PA4 | Vibration motor enable | PB3 / PB4 / PB5 | SPI1 SCK / MISO / MOSI |
| PA7 | Battery sense (ADC1_IN12) | PB6 | PN532 IRQ |
| PA9 | VBUS detect (EXTI9) | PB7 | Battery sense (external PVD input) |
| PA11 / PA12 | USB D- / D+ | PA15 | PN532 chip-select |
| PA13 / PA14 | SWDIO / SWCLK | PC14 / PC15 | 32.768 kHz crystal |

`Firmware/Card Attendance System.ioc` is the reference for the full CubeMX configuration.

### PCB

- **Board:**
  - 60 × 88.6 mm, 2 layers, 1.6 mm FR-4.
  - All 88 components are on the top side.
  - 0805 passives throughout, except the 0402 ferrite beads, plus the QFN, MSOP, SOT-23 and SMA packages.
- **Floorplan:**
  - The NFC antenna occupies the top 37 mm, in a rule area that keeps copper pours out.
  - Below it are the PN532 header, the MCU (left) and the ST25R3916 front end (centre-right).
  - The power section runs along the bottom edge: USB-C, ESD protection, charger and LDO, with the battery connector on the right edge.
- **Routing:**
  - About 1.0 m of track on the top layer and 0.35 m on the bottom, with 90 vias of 0.8 mm diameter and 0.3 mm drill.
  - Track widths run from 0.15 mm to 1.2 mm.
- **Copper pours:** GND on both layers as the return plane, plus `+5V` pours for the USB input. The QFN exposed pads are stitched to GND with vias: 2 under U1 and 3 under U5.
- **Net classes** (in `PCB/Attendance Management System.kicad_pro`):
  - Every net is in the **Default** class (0.1 mm track, 0.1 mm clearance).
  - **Power** (0.2 / 0.15 mm) and **Power2** (0.2 / 0.4 mm) are defined but not assigned to any net.
  - Tracks are drawn at explicit widths: USB D+/D- at 0.15–0.2 mm, power up to 1.2 mm.
  - Check the 0.1 mm clearance against your fab's 2-layer capability.
- **Design rules:** manufacturing rules live in `PCB/Attendance Management System.kicad_dru`:
  - global minimums for hole clearance, annular ring, hole-to-hole and courtyard clearance;
  - exceptions for the MCP73833's 0.5 mm-pitch MSOP-10 pads and the antenna crossover.
- **Schematic sheets:**
  - `power.kicad_sch`: USB-C, ESD, charger, LDO and battery.
  - `mcu.kicad_sch`: STM32, buttons, LEDs, motor driver, SWD and the PN532 header.
  - `rfid.kicad_sch`: ST25R3916, crystal, EMC filter, matching network and antenna.

## Firmware

The firmware is split along one rule: **the application logic (`App/`) decides, the drivers (`Bsp/`) act**.
- **`App/`:** holds the state machine, card decoding, flash log, student list, duplicate filter, CSV/FAT12 volume and battery maths. It includes no HAL or register headers and compiles on a PC.
- **`Bsp/`:** implements the small hardware contract in `App/Inc/platform_if.h`, handing up only raw values.
- **`Tests/`:** builds all of `App/` against a RAM-backed stub platform, so the logic is tested without hardware.

`Firmware/docs/ARCHITECTURE.md` describes the design in full: power modes, flash layout, record and CSV formats, and the decoder.

### Build and test

Run these from `Firmware/`:

```sh
make test    # host tests for the application logic; needs only a native C compiler
make         # cross-compiles build/card-attendance.{elf,hex,bin}
make clean
```

- **Toolchain:** `make` uses the GCC bundled with STM32CubeIDE (`/opt/st/stm32cubeide_*`) when one is installed, and otherwise `arm-none-eabi-gcc` on `PATH`.
- **Flashing and debugging:** use STM32CubeIDE over SWD (J3).

### Data on the device

- **Flash layout:** the top 128 kB of flash (from `0x08020000`) is reserved by the linker script.
  - Page 0: configuration.
  - Pages 1–8: the sorted student list.
  - Pages 9–63: the attendance log.
- **Records:** each record is 8 bytes (student ID and seconds since 2000-01-01), exactly one flash double-word.
- **Provisioning:** the student list is written into flash from outside, not by the firmware itself. `Firmware/Tests/test_main.c:provision_students()` shows the exact layout. The RTC starts at 2026-01-01 until a host sets it.

## Repository layout

```
Firmware/            STM32CubeIDE project + standalone Makefile
  App/               application logic (portable, host-tested)
  Bsp/               board support: HAL drivers, USB MSC glue, interrupt handlers
  Core/              CubeMX-generated startup code (edit only inside USER CODE blocks)
  Drivers/           ST HAL and CMSIS
  Tests/             host test harness
  docs/              ARCHITECTURE.md
  Card Attendance System.ioc   CubeMX pin and peripheral configuration
PCB/                 KiCad 10 project
  Attendance Management System.kicad_sch / .kicad_pcb / .kicad_pro / .kicad_dru
  power.kicad_sch, mcu.kicad_sch, rfid.kicad_sch
CLAUDE.md            detailed engineering notes (design decisions, open issues)
LICENSE              MIT
```

## Tools

- **PCB:** KiCad 10.0.
- **Firmware:** STM32CubeIDE 1.19 (CubeMX 6.15, STM32Cube FW_L4 V1.18.2), or `arm-none-eabi-gcc` with `make`.
- **Host tests:** any C11 compiler (`cc`).

## Known issues

- **The NFC antenna isn't on the board yet.** The schematic assigns AE3 the footprint `Snapeda:NFC_Loop_40x30_3T`, but the board still carries a net-tie placeholder. Run *Update PCB from Schematic*, place the loop in the antenna area, and route its feed to R21/R22.
  - Tune the matching network (C16–C19, R15) and the RX divider (C15/C37, C38/C39) against the real coil's measured inductance, resistance and capacitance. ST's AN5276 describes the procedure.
- **The PCB has footprint libraries outside the repository.** U3, U5 and the antenna footprint come from a SnapEDA library at an absolute path (`/home/rajinthan/Documents/kicad/external_libs/Snapeda.pretty`). That library is referenced in the global library table as `External` and in `PCB/fp-lib-table` as `Snapeda`, where it is listed twice. A fresh clone can't resolve those footprints until the library is added to the repository or the paths are updated.
- **The firmware doesn't build.** `Firmware/Middlewares/ST/STM32_USB_Device_Library` was removed by a CubeMX regeneration. Restore Core and Class/MSC (without the `*_template.c` files) from STM32Cube FW_L4 V1.18.2 or from git history, and re-add `Middlewares` to the source folders in `.cproject`.
- **The firmware still targets the 125 kHz EM4100 front end.** Porting it to the ST25R3916 requires:
  - an SPI driver (for example ST's RFAL);
  - the ST25R3916's capacitive wake-up;
  - the PN532 fallback;
  - a new card-ID to student-ID mapping, because ISO 14443 UIDs are 4, 7 or 10 bytes and the flash record's student ID is 32 bits;
  - setting `APP_BATT_DIV_LOW_KOHM` to 2700 to match the battery divider.
- **Charge current:** R6 = 1 kΩ sets 1 A. That's more than the 500 mA a USB-C sink with plain 5.1 kΩ CC resistors may draw, and more than the MSOP-10 charger can dissipate. Use R6 ≥ 2 kΩ.
- **PN532 placement:** a PN532 module plugged into J7 sits close to the ST25R3916 antenna. Check that it doesn't detune the loop.

## License

MIT. See [LICENSE](LICENSE).

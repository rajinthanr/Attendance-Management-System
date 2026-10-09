# Datasheets

Reference documents for the parts on the board. Designators follow the current
schematic (`kicad-cli sch export bom`).

| File | Part | Used for |
|---|---|---|
| `STM32L432KB_DS11451_Rev4.pdf` | STM32L432KB (U4), MCU | Stop 2 / Standby wake-up times, run currents, clocks |
| `ST25R3916_DS12484_Rev8.pdf` | ST25R3916 (U5), NFC reader | Wake-up mode, timers, supply currents, SPI |
| `AN4555_STM32L4_hardware_development.pdf` | STM32L4 application note | Power supply, reset, clocks, boot, SWD |
| `FC-135_32.768kHz_crystal_Y1_Q13FC13500049.pdf` | Epson FC-135 (Y1), LSE crystal | CL 6 pF, ±20 ppm, R1 ≤ 70 kΩ, C0 1.2 pF |
| `X32252712MMB4SI_27.12MHz_crystal_Y2.pdf` | YXC 27.12 MHz (Y2), reader crystal | CL 10 pF |
| `MCP73833_charger_DS20002005.pdf` | MCP73833 (U2), Li-ion charger | PROG resistor, status pins |
| `TPS7A02_LDO.pdf` | TPS7A0233 (U3), 3.3 V LDO | Quiescent current, dropout |
| `USBLC6-2SC6_ESD.pdf` | USBLC6-2SC6 (U1), USB ESD | Layout next to J1 |
| `AO3400A_motor_MOSFET.pdf` | AO3400A (Q1), motor switch | Gate threshold at 3.3 V |
| `SS14_flyback_diode.pdf` | SS14 (D4), motor flyback | |

Not included: st.com refuses scripted downloads. Get these from a browser:

- RM0394, STM32L41x-46x reference manual (Stop 2, LPTIM, RCC, PWR):
  https://www.st.com/resource/en/reference_manual/rm0394-stm32l41xxx42xxx43xxx44xxx45xxx46xxx-advanced-armbased-32bit-mcus-stmicroelectronics.pdf
- AN2867, oscillator design guide (LSE drive level):
  https://www.st.com/resource/en/application_note/an2867-guidelines-for-oscillator-design-on-stm8afals-and-stm32-mcusmpus-stmicroelectronics.pdf
- AN5276, antenna matching for ST25 NFC readers:
  https://www.st.com/resource/en/application_note/an5276-antenna-matching-for-st25-nfc-readers-stmicroelectronics.pdf

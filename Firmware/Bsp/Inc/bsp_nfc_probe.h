#ifndef BSP_NFC_PROBE_H
#define BSP_NFC_PROBE_H

#include <stdbool.h>
#include <stdint.h>

#define BSP_NFC_UID_MAX_BYTES 10u

/* Temporary ST25R3916 antenna probe for the boot LED diagnostic.
 * supply_mv is the battery voltage feeding the reader's VDD/VDD_TX pins. */
bool bsp_nfc_probe_init(uint32_t supply_mv);
bool bsp_nfc_probe_amplitude(uint8_t *amplitude);
/* Poll one ISO14443-A card. Returns the UID bytes and length on success. */
bool bsp_nfc_poll_uid(uint8_t uid[BSP_NFC_UID_MAX_BYTES], uint8_t *uid_length);

/* Visible in Live Expressions when a probe initialization step fails.
 * init_step: 0=not started, 1=SPI ready, 2=ID verified, 3=trim written,
 * 4=supply/I/O configured, 5=oscillator stable, 6=regulators adjusted. */
extern volatile uint8_t bsp_nfc_probe_chip_id;
extern volatile uint8_t bsp_nfc_probe_init_step;
/* Poll progress: 0=entry, 1=field configured, 2=ATQA, 3=anticollision,
 * 4=SELECT response, 5=UID complete. */
extern volatile uint8_t bsp_nfc_poll_step;
/* Poll fail codes: 0=none, 1=not initialized/invalid args, 2=ISO-A setup,
 * 3=field reset, 4=field on, 5=REQA setup, 6=REQA command,
 * 7=ATQA timeout/error, 8=ATQA FIFO, 9=anticollision response,
 * 10=UID BCC mismatch, 11=SELECT/SAK; last IRQ bytes show observed flags. */
extern volatile uint8_t bsp_nfc_poll_fail_code;
extern volatile uint8_t bsp_nfc_poll_irq_main;
extern volatile uint8_t bsp_nfc_poll_irq_error;
extern volatile uint8_t bsp_nfc_poll_irq_main_seen;
extern volatile uint8_t bsp_nfc_poll_irq_error_seen;
/* RX wait result: 0=not run, 1=SPI read failure, 2=collision,
 * 3=framing/CRC/parity error, 4=RX complete, 5=timeout. */
extern volatile uint8_t bsp_nfc_poll_rx_wait_result;
extern volatile uint8_t bsp_nfc_poll_atqa[2];

#endif /* BSP_NFC_PROBE_H */

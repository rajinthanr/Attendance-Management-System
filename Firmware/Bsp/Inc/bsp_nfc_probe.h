#ifndef BSP_NFC_PROBE_H
#define BSP_NFC_PROBE_H

#include <stdbool.h>
#include <stdint.h>

/* Temporary ST25R3916 antenna probe for the boot LED diagnostic. */
bool bsp_nfc_probe_init(void);
bool bsp_nfc_probe_amplitude(uint8_t *amplitude);

#endif /* BSP_NFC_PROBE_H */

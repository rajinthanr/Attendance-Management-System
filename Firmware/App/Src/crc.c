/**
 * @file    crc.c
 * @brief   Level 2 (logic) — bitwise CRC implementations.
 *
 * Bitwise rather than table driven: the tables would cost 512 B of flash for
 * a few microseconds saved on buffers that are at most a couple of kilobytes,
 * and these only run at page-close and flush time.
 */
#include "crc.h"

uint16_t crc16_ccitt_update(uint16_t crc, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t i;

    while (len-- > 0u) {
        crc ^= (uint16_t)((uint16_t)*p++ << 8);
        for (i = 0u; i < 8u; i++) {
            crc = (crc & 0x8000u) ? (uint16_t)(((uint32_t)crc << 1) ^ 0x1021u)
                                  : (uint16_t)((uint32_t)crc << 1);
        }
    }
    return crc;
}

uint16_t crc16_ccitt(const void *data, uint32_t len)
{
    return crc16_ccitt_update(0xFFFFu, data, len);
}

uint32_t crc32_ieee_update(uint32_t crc, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t i;

    while (len-- > 0u) {
        crc ^= (uint32_t)*p++;
        for (i = 0u; i < 8u; i++) {
            /* Reflected form, so the polynomial is shifted right. */
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc;
}

uint32_t crc32_ieee(const void *data, uint32_t len)
{
    return crc32_ieee_update(0xFFFFFFFFu, data, len) ^ 0xFFFFFFFFu;
}

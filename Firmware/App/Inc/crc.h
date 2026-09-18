/**
 * @file    crc.h
 * @brief   Level 2 (logic) — software CRCs.
 *
 * Deliberately not the STM32 CRC peripheral: these run over data that is
 * about to be written to flash during a low-battery flush, when the fewest
 * possible clocks should be enabled. They are also what makes the log format
 * verifiable off-target by the host tests.
 */
#ifndef CRC_H
#define CRC_H

#include "app_types.h"

/** CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no xorout. */
uint16_t crc16_ccitt(const void *data, uint32_t len);

/** Continue a CRC-16 across a discontiguous buffer. */
uint16_t crc16_ccitt_update(uint16_t seed, const void *data, uint32_t len);

/** CRC-32/ISO-HDLC (zlib): poly 0x04C11DB7 reflected, init/xorout 0xFFFFFFFF. */
uint32_t crc32_ieee(const void *data, uint32_t len);
uint32_t crc32_ieee_update(uint32_t seed, const void *data, uint32_t len);

#endif /* CRC_H */

/**
 * @file    bsp_flash.c
 * @brief   Level 1 (HAL) — internal flash access for the data region.
 *
 * Offsets arriving from Level 2 are relative to the region, never absolute.
 * Every entry point range-checks against the region, so a bug in the log
 * format cannot reach the vector table or the code below it.
 */
#include "bsp.h"

/** True when [offset, offset+len) lies inside the data region. */
static bool in_region(uint32_t offset, uint32_t len)
{
    if (len > BSP_FLASH_SIZE) {
        return false;
    }
    return (offset <= (BSP_FLASH_SIZE - len));
}

uint32_t plat_flash_page_size(void)
{
    return BSP_FLASH_PAGE_SIZE;
}

bool plat_flash_read(uint32_t offset, void *dst, uint32_t len)
{
    if (dst == NULL || !in_region(offset, len)) {
        return false;
    }

    /* Flash is memory mapped, so a read is a copy. Byte-wise because the
     * caller's destination has no guaranteed alignment. */
    const uint8_t *src = (const uint8_t *)(BSP_FLASH_BASE + offset);
    uint8_t *out = (uint8_t *)dst;

    while (len-- > 0u) {
        *out++ = *src++;
    }
    return true;
}

bool plat_flash_write_dw(uint32_t offset, uint64_t value)
{
    if ((offset % 8u) != 0u || !in_region(offset, 8u)) {
        return false;
    }

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return false;
    }

    /* Clear the sticky error flags: the L4 refuses to program while any of
     * them is set, and one can be left over from an interrupted write. */
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);

    HAL_StatusTypeDef st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                                             BSP_FLASH_BASE + offset,
                                             value);
    (void)HAL_FLASH_Lock();

    return (st == HAL_OK);
}

bool plat_flash_erase(uint32_t offset)
{
    if (!in_region(offset, 1u)) {
        return false;
    }

    FLASH_EraseInitTypeDef e = { 0 };
    uint32_t error = 0u;

    e.TypeErase = FLASH_TYPEERASE_PAGES;
    e.Banks = FLASH_BANK_1;
    e.Page = BSP_FLASH_FIRST_PAGE + (offset / BSP_FLASH_PAGE_SIZE);
    e.NbPages = 1u;

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return false;
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);

    HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&e, &error);

    (void)HAL_FLASH_Lock();

    return (st == HAL_OK) && (error == 0xFFFFFFFFu);
}

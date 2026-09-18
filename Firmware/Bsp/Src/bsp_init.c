/**
 * @file    bsp_init.c
 * @brief   Level 1 (HAL) — board bring-up order.
 */
#include "bsp.h"

void bsp_init(void)
{
    /* Order matters. Clocks first, because everything below needs them; power
     * last, because the PVD must not fire before the application can act on
     * it, and its reset-cause capture has to happen before anything else
     * clears the RCC flags. */
    bsp_clock_init();
    bsp_gpio_init();
    bsp_time_init();
    bsp_rf_init();
    bsp_adc_init();
    bsp_power_init();
}

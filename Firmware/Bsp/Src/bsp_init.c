/**
 * @file    bsp_init.c
 * @brief   Level 1 (HAL) — board bring-up order.
 */
#include "bsp.h"

void bsp_init(void)
{
    /* Order matters. Clocks first, because everything below needs them; power
     * last, because its reset-cause capture reads the RCC flags before they
     * are cleared. The reader is brought up by the application, which picks
     * its supply mode from the battery voltage. */
    bsp_clock_init();
    bsp_gpio_init();
    bsp_time_init();
    bsp_adc_init();
    bsp_power_init();
}

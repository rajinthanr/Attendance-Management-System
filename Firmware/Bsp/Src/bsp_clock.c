/**
 * @file    bsp_clock.c
 * @brief   Level 1 (HAL) — clock tree.
 *
 * Two operating points. Scanning runs the core from MSI at 4 MHz, the slowest
 * setting that still keeps flash latency at zero and decodes a capture between
 * frames. USB needs more, so the session brings the core to 24 MHz and drops
 * back on unplug.
 *
 * The LSE runs continuously and clocks the RTC and both LPTIMs, which is what
 * lets every timer in the design survive Stop 2.
 */
#include "bsp.h"

static void clock_config(uint32_t msi_range, uint32_t latency)
{
    RCC_OscInitTypeDef osc = { 0 };
    RCC_ClkInitTypeDef clk = { 0 };

    /* Flash wait states are not touched here. When MSI is already the system
     * clock, HAL_RCC_OscConfig raises the latency itself before widening the
     * MSI range, and HAL_RCC_ClockConfig lowers it after narrowing one, so
     * the core is never faster than the configured wait states allow. */
    osc.OscillatorType = RCC_OSCILLATORTYPE_MSI | RCC_OSCILLATORTYPE_LSE;
    osc.MSIState = RCC_MSI_ON;
    osc.MSICalibrationValue = RCC_MSICALIBRATION_DEFAULT;
    osc.MSIClockRange = msi_range;
    osc.LSEState = RCC_LSE_ON;
    osc.PLL.PLLState = RCC_PLL_NONE;

    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        Error_Handler();
    }

    clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                    RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_MSI;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV1;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;

    if (HAL_RCC_ClockConfig(&clk, latency) != HAL_OK) {
        Error_Handler();
    }
}

/** Route the peripherals that do not run from PCLK. */
static void periph_clock_config(bool usb)
{
    RCC_PeriphCLKInitTypeDef p = { 0 };

    p.PeriphClockSelection = RCC_PERIPHCLK_RTC |
                             RCC_PERIPHCLK_LPTIM1 |
                             RCC_PERIPHCLK_LPTIM2 |
                             RCC_PERIPHCLK_ADC;
    p.RTCClockSelection = RCC_RTCCLKSOURCE_LSE;
    p.Lptim1ClockSelection = RCC_LPTIM1CLKSOURCE_LSE;
    p.Lptim2ClockSelection = RCC_LPTIM2CLKSOURCE_LSE;
    p.AdcClockSelection = RCC_ADCCLKSOURCE_SYSCLK;

    if (usb) {
        /* Crystal-less USB: HSI48 feeds the peripheral and the CRS trims it
         * against the host's start-of-frame, which holds it inside the
         * +/-0.25 % that full speed requires without a second crystal. */
        p.PeriphClockSelection |= RCC_PERIPHCLK_USB;
        p.UsbClockSelection = RCC_USBCLKSOURCE_HSI48;
    }

    if (HAL_RCCEx_PeriphCLKConfig(&p) != HAL_OK) {
        Error_Handler();
    }
}

/**
 * Stop the oscillators the boot code may have left running.
 *
 * CubeMX's generated SystemClock_Config() runs before bsp_init() and brings
 * the part up on an 80 MHz PLL. clock_config() moves SYSCLK back to MSI, but
 * it passes RCC_PLL_NONE, which tells the HAL to leave the PLL alone — so
 * without this the PLL would keep running for the life of the device. On a
 * battery part that is the difference between microamps and milliamps.
 *
 * Only safe once SYSCLK is already MSI: the HAL refuses to stop a PLL that is
 * driving the core.
 */
static void stop_unused_oscillators(void)
{
    RCC_OscInitTypeDef osc = { 0 };

    osc.OscillatorType = RCC_OSCILLATORTYPE_HSI48;
    osc.HSI48State = RCC_HSI48_OFF;
    osc.PLL.PLLState = RCC_PLL_OFF;
    (void)HAL_RCC_OscConfig(&osc);
}

void bsp_clock_init(void)
{
    /* Scale 1 is needed for the 24 MHz USB operating point; staying there
     * avoids a regulator transition on every plug event. */
    if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK) {
        Error_Handler();
    }

    /* The backup domain holds the LSE and the RTC; it is write protected out
     * of reset and stays enabled from here on. */
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();

    clock_config(BSP_MSI_RANGE_RUN, FLASH_LATENCY_0);
    stop_unused_oscillators();
    periph_clock_config(false);
}

void bsp_clock_set_usb_speed(bool fast)
{
    if (fast) {
        RCC_OscInitTypeDef osc = { 0 };

        osc.OscillatorType = RCC_OSCILLATORTYPE_HSI48;
        osc.HSI48State = RCC_HSI48_ON;
        osc.PLL.PLLState = RCC_PLL_NONE;
        if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
            Error_Handler();
        }

        clock_config(BSP_MSI_RANGE_USB, FLASH_LATENCY_1);
        periph_clock_config(true);
    } else {
        clock_config(BSP_MSI_RANGE_RUN, FLASH_LATENCY_0);
        periph_clock_config(false);

        /* The carrier period and the capture prescaler were both derived from
         * BSP_SYSCLK_RUN_HZ. Re-deriving them here keeps the reader correct
         * for any path that resumes scanning after a USB session, rather than
         * relying on the fact that today's only such path goes via Standby
         * and therefore a reset. */
        bsp_rf_init();

        /* HSI48 is the largest consumer left once the host is gone. */
        RCC_OscInitTypeDef osc = { 0 };
        osc.OscillatorType = RCC_OSCILLATORTYPE_HSI48;
        osc.HSI48State = RCC_HSI48_OFF;
        osc.PLL.PLLState = RCC_PLL_NONE;
        (void)HAL_RCC_OscConfig(&osc);
    }
}

void bsp_clock_restore(void)
{
    /* Stop 2 always exits on MSI at its pre-Stop range, and the RCC config
     * registers survive, so the tree only needs re-selecting if something
     * other than MSI was driving SYSCLK. With MSI as the only source that is
     * never the case, and this reduces to confirming the range. */
    if (__HAL_RCC_GET_SYSCLK_SOURCE() != RCC_SYSCLKSOURCE_STATUS_MSI) {
        clock_config(BSP_MSI_RANGE_RUN, FLASH_LATENCY_0);
    }
}

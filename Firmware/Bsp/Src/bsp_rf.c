/**
 * @file    bsp_rf.c
 * @brief   Level 1 (HAL) — 125 kHz carrier and envelope capture.
 *
 * TIM1 drives the antenna tank. TIM2 timestamps every edge of the demodulated
 * envelope straight into RAM over DMA, so the core has nothing to do during a
 * read and can sit in Sleep mode for the whole 100 ms.
 *
 * No interpretation happens here. The array of raw timer ticks goes up to
 * em4100.c exactly as the hardware produced it.
 */
#include "bsp.h"
#include "app_events.h"

TIM_HandleTypeDef hbsp_tim_carrier;
TIM_HandleTypeDef hbsp_tim_capture;
DMA_HandleTypeDef hbsp_dma_capture;

static uint16_t s_capacity;
static bool     s_running;

/* ------------------------------------------------------------------------ */
/* Init                                                                     */
/* ------------------------------------------------------------------------ */

static void carrier_init(void)
{
    TIM_OC_InitTypeDef oc = { 0 };
    TIM_MasterConfigTypeDef master = { 0 };

    __HAL_RCC_TIM1_CLK_ENABLE();

    /* SYSCLK / (ARR + 1) = 125 kHz. At 4 MHz that is 32 counts, giving a
     * 50 % square wave with a compare of 16. */
    hbsp_tim_carrier.Instance = BSP_CARRIER_TIM;
    hbsp_tim_carrier.Init.Prescaler = 0u;
    hbsp_tim_carrier.Init.CounterMode = TIM_COUNTERMODE_UP;
    hbsp_tim_carrier.Init.Period = (BSP_SYSCLK_RUN_HZ / BSP_RF_CARRIER_HZ) - 1u;
    hbsp_tim_carrier.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    hbsp_tim_carrier.Init.RepetitionCounter = 0u;
    hbsp_tim_carrier.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

    if (HAL_TIM_PWM_Init(&hbsp_tim_carrier) != HAL_OK) {
        Error_Handler();
    }

    master.MasterOutputTrigger = TIM_TRGO_RESET;
    master.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
    (void)HAL_TIMEx_MasterConfigSynchronization(&hbsp_tim_carrier, &master);

    oc.OCMode = TIM_OCMODE_PWM1;
    oc.Pulse = (hbsp_tim_carrier.Init.Period + 1u) / 2u;
    oc.OCPolarity = TIM_OCPOLARITY_HIGH;
    oc.OCNPolarity = TIM_OCNPOLARITY_HIGH;
    oc.OCFastMode = TIM_OCFAST_DISABLE;
    oc.OCIdleState = TIM_OCIDLESTATE_RESET;
    oc.OCNIdleState = TIM_OCNIDLESTATE_RESET;

    if (HAL_TIM_PWM_ConfigChannel(&hbsp_tim_carrier, &oc, BSP_CARRIER_CHANNEL) != HAL_OK) {
        Error_Handler();
    }
}

static void capture_init(void)
{
    TIM_IC_InitTypeDef ic = { 0 };
    TIM_ClockConfigTypeDef clk = { 0 };

    __HAL_RCC_TIM2_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();

    hbsp_tim_capture.Instance = BSP_CAPTURE_TIM;
    hbsp_tim_capture.Init.Prescaler = (BSP_SYSCLK_RUN_HZ / BSP_CAPTURE_HZ) - 1u;
    hbsp_tim_capture.Init.CounterMode = TIM_COUNTERMODE_UP;
    hbsp_tim_capture.Init.Period = 0xFFFFFFFFu;   /* 32-bit: no wrap in 100 ms */
    hbsp_tim_capture.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    hbsp_tim_capture.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

    if (HAL_TIM_IC_Init(&hbsp_tim_capture) != HAL_OK) {
        Error_Handler();
    }

    clk.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
    (void)HAL_TIM_ConfigClockSource(&hbsp_tim_capture, &clk);

    /* Both edges: Manchester carries its information in the transitions, and
     * the decoder needs the interval between every one of them. */
    ic.ICPolarity = TIM_INPUTCHANNELPOLARITY_BOTHEDGE;
    ic.ICSelection = TIM_ICSELECTION_DIRECTTI;
    ic.ICPrescaler = TIM_ICPSC_DIV1;
    /* Input filter of 8 samples at fDTS/1: rejects the 125 kHz carrier
     * breaking through the envelope detector without touching the 256 us
     * half-bit it has to preserve. */
    ic.ICFilter = 8u;

    if (HAL_TIM_IC_ConfigChannel(&hbsp_tim_capture, &ic, BSP_CAPTURE_CHANNEL) != HAL_OK) {
        Error_Handler();
    }

    hbsp_dma_capture.Instance = BSP_CAPTURE_DMA_CH;
    hbsp_dma_capture.Init.Request = BSP_CAPTURE_DMA_REQ;
    hbsp_dma_capture.Init.Direction = DMA_PERIPH_TO_MEMORY;
    hbsp_dma_capture.Init.PeriphInc = DMA_PINC_DISABLE;
    hbsp_dma_capture.Init.MemInc = DMA_MINC_ENABLE;
    hbsp_dma_capture.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
    hbsp_dma_capture.Init.MemDataAlignment = DMA_MDATAALIGN_WORD;
    hbsp_dma_capture.Init.Mode = DMA_NORMAL;
    hbsp_dma_capture.Init.Priority = DMA_PRIORITY_HIGH;

    if (HAL_DMA_Init(&hbsp_dma_capture) != HAL_OK) {
        Error_Handler();
    }

    __HAL_LINKDMA(&hbsp_tim_capture, hdma[TIM_DMA_ID_CC2], hbsp_dma_capture);

    HAL_NVIC_SetPriority(BSP_CAPTURE_DMA_IRQ, BSP_PRIO_CAPTURE_DMA, 0u);
    HAL_NVIC_EnableIRQ(BSP_CAPTURE_DMA_IRQ);
}

void bsp_rf_init(void)
{
    carrier_init();
    capture_init();
    s_running = false;
}

/* ------------------------------------------------------------------------ */
/* platform_if                                                              */
/* ------------------------------------------------------------------------ */

void plat_rf_power(bool on)
{
    HAL_GPIO_WritePin(PORT_RF_PWR_EN, PIN_RF_PWR_EN,
                      on ? GPIO_PIN_SET : GPIO_PIN_RESET);

    if (on) {
        GPIO_InitTypeDef g = { 0 };

        /* The carrier and data pins are only driven while the front end has
         * power. Left as AF with the rail down they would leak into it. */
        g.Mode = GPIO_MODE_AF_PP;
        g.Pull = GPIO_NOPULL;
        g.Speed = GPIO_SPEED_FREQ_HIGH;
        g.Alternate = BSP_CARRIER_AF;
        g.Pin = PIN_RF_CARRIER;
        HAL_GPIO_Init(PORT_RF_CARRIER, &g);

        g.Mode = GPIO_MODE_AF_PP;
        g.Speed = GPIO_SPEED_FREQ_LOW;
        g.Alternate = BSP_CAPTURE_AF;
        g.Pin = PIN_RF_DATA;
        HAL_GPIO_Init(PORT_RF_DATA, &g);
    } else {
        GPIO_InitTypeDef g = { 0 };

        g.Mode = GPIO_MODE_ANALOG;
        g.Pull = GPIO_NOPULL;
        g.Pin = PIN_RF_CARRIER;
        HAL_GPIO_Init(PORT_RF_CARRIER, &g);
        g.Pin = PIN_RF_DATA;
        HAL_GPIO_Init(PORT_RF_DATA, &g);
    }
}

void plat_rf_carrier(bool on)
{
    if (on) {
        (void)HAL_TIM_PWM_Start(&hbsp_tim_carrier, BSP_CARRIER_CHANNEL);

        /* The tank needs a few milliseconds of drive to ring up, and the tag
         * needs that field to power its own oscillator before it transmits
         * anything. Capturing before this has elapsed just fills the buffer
         * with the turn-on transient. */
        HAL_Delay(BSP_RF_SETTLE_MS);
    } else {
        (void)HAL_TIM_PWM_Stop(&hbsp_tim_carrier, BSP_CARRIER_CHANNEL);
    }
}

void plat_rf_capture_start(uint32_t *buf, uint16_t cap)
{
    s_capacity = cap;
    s_running = true;

    __HAL_TIM_SET_COUNTER(&hbsp_tim_capture, 0u);
    (void)HAL_TIM_IC_Start_DMA(&hbsp_tim_capture, BSP_CAPTURE_CHANNEL, buf, cap);
}

uint16_t plat_rf_capture_stop(void)
{
    uint16_t collected = 0u;

    if (s_running) {
        /* Whatever the DMA has not yet transferred is what is left of the
         * request, so the difference is the number of edges captured. */
        uint32_t remaining = __HAL_DMA_GET_COUNTER(&hbsp_dma_capture);

        collected = (remaining >= s_capacity)
                  ? 0u
                  : (uint16_t)(s_capacity - remaining);

        (void)HAL_TIM_IC_Stop_DMA(&hbsp_tim_capture, BSP_CAPTURE_CHANNEL);
        s_running = false;
    }
    return collected;
}

uint32_t plat_rf_capture_hz(void)
{
    return BSP_CAPTURE_HZ;
}

/* ------------------------------------------------------------------------ */
/* DMA completion: the buffer filled before the read timed out              */
/* ------------------------------------------------------------------------ */

void HAL_TIM_IC_CaptureCallback(TIM_HandleTypeDef *h)
{
    if (h->Instance == BSP_CAPTURE_TIM) {
        app_event_post(APP_EVT_CAPTURE_FULL);
    }
}

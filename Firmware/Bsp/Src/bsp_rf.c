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
#include "bsp_nfc_probe.h"
#include <string.h>

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

/* Temporary ST25R3916 antenna probe used by the boot LED diagnostic. */
extern SPI_HandleTypeDef hspi1;

#define NFC_REG_IO_CONF1        0x00u
#define NFC_REG_IO_CONF2        0x01u
#define NFC_REG_OPERATION       0x02u
#define NFC_REG_IRQ_MAIN        0x1Au
#define NFC_REG_IRQ_TIMER       0x1Bu
#define NFC_REG_ADC_OUTPUT      0x25u
#define NFC_REG_REGULATOR       0x2Cu
#define NFC_REG_MODE            0x03u
#define NFC_REG_BIT_RATE        0x04u
#define NFC_REG_ISO_A           0x05u
#define NFC_REG_FIFO_STATUS1    0x1Eu
#define NFC_REG_FIFO_STATUS2    0x1Fu
#define NFC_REG_NUM_TX1         0x22u
#define NFC_REG_NUM_TX2         0x23u
#define NFC_REG_ID              0x3Fu
#define NFC_CMD_SET_DEFAULT     0xC1u
#define NFC_CMD_STOP_ALL        0xC2u
#define NFC_CMD_TX_CRC          0xC4u
#define NFC_CMD_TX_NO_CRC       0xC5u
#define NFC_CMD_TX_REQA         0xC6u
#define NFC_CMD_CLEAR_FIFO      0xDBu
#define NFC_CMD_MEASURE_AMPL    0xD3u
#define NFC_CMD_RESET_RX_GAIN   0xD5u
#define NFC_CMD_ADJUST_REG      0xD6u
#define NFC_IRQ_OSC_STABLE      0x80u
#define NFC_IRQ_CMD_DONE        0x80u
#define NFC_SPI_TIMEOUT_MS      10u
#define NFC_OSC_TIMEOUT_MS      50u
#define NFC_RX_TIMEOUT_MS       5u

static bool s_nfc_typea_ready;

volatile uint8_t bsp_nfc_probe_chip_id;
volatile uint8_t bsp_nfc_probe_init_step;
volatile uint8_t bsp_nfc_poll_step;
volatile uint8_t bsp_nfc_poll_fail_code;
volatile uint8_t bsp_nfc_poll_irq_main;
volatile uint8_t bsp_nfc_poll_irq_error;
volatile uint8_t bsp_nfc_poll_irq_main_seen;
volatile uint8_t bsp_nfc_poll_irq_error_seen;
volatile uint8_t bsp_nfc_poll_rx_wait_result;
volatile uint8_t bsp_nfc_poll_atqa[2];

static bool nfc_spi_init(void)
{
    /* ST25R3916: CPOL=0, CPHA=1, MSB first, 8 bits, software BSS.
     * At the 4 MHz boot PCLK, /8 gives 500 kHz, below the 10 MHz limit. */
    hspi1.Instance = SPI1;
    hspi1.Init.Mode = SPI_MODE_MASTER;
    hspi1.Init.Direction = SPI_DIRECTION_2LINES;
    hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi1.Init.CLKPhase = SPI_PHASE_2EDGE;
    hspi1.Init.NSS = SPI_NSS_SOFT;
    hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_8;
    hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    hspi1.Init.CRCPolynomial = 7u;
    hspi1.Init.CRCLength = SPI_CRC_LENGTH_DATASIZE;
    hspi1.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
    return HAL_SPI_Init(&hspi1) == HAL_OK;
}

static bool nfc_spi_write(const uint8_t *bytes, uint16_t length)
{
    HAL_GPIO_WritePin(PORT_NFC_NSS, PIN_NFC_NSS, GPIO_PIN_RESET);
    HAL_StatusTypeDef status = HAL_SPI_Transmit(&hspi1, (uint8_t *)bytes,
                                                length, NFC_SPI_TIMEOUT_MS);
    HAL_GPIO_WritePin(PORT_NFC_NSS, PIN_NFC_NSS, GPIO_PIN_SET);
    return status == HAL_OK;
}

static bool nfc_read(uint8_t reg, uint8_t *value)
{
    uint8_t tx[2] = { (uint8_t)(0x40u | reg), 0u };
    uint8_t rx[2] = { 0u, 0u };

    HAL_GPIO_WritePin(PORT_NFC_NSS, PIN_NFC_NSS, GPIO_PIN_RESET);
    HAL_StatusTypeDef status = HAL_SPI_TransmitReceive(&hspi1, tx, rx, 2u,
                                                       NFC_SPI_TIMEOUT_MS);
    HAL_GPIO_WritePin(PORT_NFC_NSS, PIN_NFC_NSS, GPIO_PIN_SET);
    if (status != HAL_OK) {
        return false;
    }
    *value = rx[1];
    return true;
}

static bool nfc_write(uint8_t reg, uint8_t value)
{
    uint8_t tx[2] = { reg, value };
    return nfc_spi_write(tx, 2u);
}

static bool nfc_command(uint8_t command)
{
    return nfc_spi_write(&command, 1u);
}

static bool nfc_fifo_write(const uint8_t *bytes, uint8_t length)
{
    uint8_t tx[8];

    if (bytes == NULL || length == 0u || length > 7u) {
        return false;
    }
    tx[0] = 0x80u; /* SPI FIFO load operation. */
    memcpy(&tx[1], bytes, length);
    return nfc_spi_write(tx, (uint16_t)length + 1u);
}

static bool nfc_fifo_read(uint8_t *bytes, uint8_t length)
{
    uint8_t tx[8] = { 0x9Fu };
    uint8_t rx[8] = { 0u };

    if (bytes == NULL || length == 0u || length > 7u) {
        return false;
    }
    HAL_GPIO_WritePin(PORT_NFC_NSS, PIN_NFC_NSS, GPIO_PIN_RESET);
    HAL_StatusTypeDef status = HAL_SPI_TransmitReceive(&hspi1, tx, rx,
                                                       (uint16_t)length + 1u,
                                                       NFC_SPI_TIMEOUT_MS);
    HAL_GPIO_WritePin(PORT_NFC_NSS, PIN_NFC_NSS, GPIO_PIN_SET);
    if (status != HAL_OK) {
        return false;
    }
    memcpy(bytes, &rx[1], length);
    return true;
}

static bool nfc_fifo_length(uint16_t *length)
{
    uint8_t low = 0u;
    uint8_t high = 0u;

    if (length == NULL || !nfc_read(NFC_REG_FIFO_STATUS1, &low) ||
        !nfc_read(NFC_REG_FIFO_STATUS2, &high) || (high & 0x30u) != 0u) {
        return false;
    }
    /* FIFO_STATUS2[7:6] are fifo_b9:fifo_b8; [5:4] are error flags. */
    *length = (uint16_t)low |
              (uint16_t)((uint16_t)(high & 0xC0u) << 2);
    return true;
}

static uint16_t nfc_crc_a(const uint8_t *bytes, uint8_t length)
{
    uint16_t crc = 0x6363u;
    uint8_t i;

    for (i = 0u; i < length; i++) {
        uint8_t value = (uint8_t)(bytes[i] ^ (uint8_t)crc);
        value = (uint8_t)(value ^ (uint8_t)(value << 4));
        crc = (uint16_t)((crc >> 8) ^ ((uint16_t)value << 8) ^
                         ((uint16_t)value << 3) ^ ((uint16_t)value >> 4));
    }
    return crc;
}

static bool nfc_wait_rx(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    bsp_nfc_poll_rx_wait_result = 0u;

    do {
        uint8_t irq_main = 0u;
        uint8_t irq_error = 0u;
        if (!nfc_read(NFC_REG_IRQ_MAIN, &irq_main) ||
            !nfc_read(0x1Cu, &irq_error)) {
            bsp_nfc_poll_rx_wait_result = 1u;
            return false;
        }
        bsp_nfc_poll_irq_main = irq_main;
        bsp_nfc_poll_irq_error = irq_error;
        bsp_nfc_poll_irq_main_seen |= irq_main;
        bsp_nfc_poll_irq_error_seen |= irq_error;
        if ((irq_main & 0x04u) != 0u) {
            bsp_nfc_poll_rx_wait_result = 2u;
            return false;
        }
        if ((irq_error & 0xF0u) != 0u) {
            bsp_nfc_poll_rx_wait_result = 3u;
            return false;
        }
        if ((irq_main & 0x10u) != 0u) {
            bsp_nfc_poll_rx_wait_result = 4u;
            return true;
        }
    } while ((uint32_t)(HAL_GetTick() - start) < timeout_ms);
    bsp_nfc_poll_rx_wait_result = 5u;
    return false;
}

static bool nfc_typea_transceive(const uint8_t *tx, uint8_t tx_length,
                                 bool append_crc, uint8_t *rx,
                                 uint8_t rx_capacity, uint8_t *rx_length)
{
    uint16_t received = 0u;

    if (tx == NULL || rx == NULL || rx_length == NULL || tx_length == 0u ||
        tx_length > 7u || rx_capacity == 0u ||
        !nfc_command(NFC_CMD_STOP_ALL) ||
        !nfc_command(NFC_CMD_RESET_RX_GAIN) ||
        !nfc_command(NFC_CMD_CLEAR_FIFO) ||
        !nfc_write(NFC_REG_NUM_TX1, 0u) ||
        !nfc_write(NFC_REG_NUM_TX2, (uint8_t)(tx_length << 3)) ||
        !nfc_fifo_write(tx, tx_length)) {
        return false;
    }
    if (!nfc_command(append_crc ? NFC_CMD_TX_CRC : NFC_CMD_TX_NO_CRC) ||
        !nfc_wait_rx(NFC_RX_TIMEOUT_MS) || !nfc_fifo_length(&received) ||
        received == 0u || received > rx_capacity || received > UINT8_MAX ||
        !nfc_fifo_read(rx, (uint8_t)received)) {
        return false;
    }
    *rx_length = (uint8_t)received;
    return true;
}

static bool nfc_wait_irq(uint8_t reg, uint8_t mask, uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    uint8_t irq = 0u;

    do {
        if (!nfc_read(reg, &irq)) {
            return false;
        }
        if ((irq & mask) != 0u) {
            return true;
        }
    } while ((uint32_t)(HAL_GetTick() - start) < timeout_ms);
    return false;
}

bool bsp_nfc_probe_init(uint32_t supply_mv)
{
    uint8_t id = 0u;
    uint8_t ignored = 0u;
    const uint8_t overheat_trim[3] = { 0xFCu, 0x04u, 0x10u };

    bsp_nfc_probe_chip_id = 0u;
    bsp_nfc_probe_init_step = 0u;
    s_nfc_typea_ready = false;
    if (supply_mv < 2400u || supply_mv > 5500u || !nfc_spi_init()) {
        return false;
    }
    bsp_nfc_probe_init_step = 1u;

    /* Bits 7:3 are 00101 for ST25R3916/7; bits 2:0 are the silicon revision. */
    if (!nfc_read(NFC_REG_ID, &id)) {
        return false;
    }
    bsp_nfc_probe_chip_id = id;
    if ((id & 0xF8u) != 0x28u) {
        return false;
    }
    bsp_nfc_probe_init_step = 2u;
    if (!nfc_command(NFC_CMD_SET_DEFAULT)) {
        return false;
    }
    /* DS12484 4.1: required after power-on and every Set Default. The three
     * bytes form a single SPI frame with BSS held low throughout. */
    if (!nfc_spi_write(overheat_trim, 3u)) {
        return false;
    }
    bsp_nfc_probe_init_step = 3u;
    /* MCU_CLK is unused on this board; keep its HF and LF outputs disabled. */
    if (!nfc_write(NFC_REG_IO_CONF1, 0x07u) ||
        /* sup3V must follow the BAT+ level, not the 3.3 V MCU I/O rail. */
        !nfc_write(NFC_REG_IO_CONF2, supply_mv <= 3600u ? 0x80u : 0x00u)) {
        return false;
    }
    bsp_nfc_probe_init_step = 4u;
    /* Ready mode: oscillator on, receiver and continuous RF field off. */
    if (!nfc_write(NFC_REG_OPERATION, 0x80u)) {
        return false;
    }
    if (!nfc_wait_irq(NFC_REG_IRQ_MAIN, NFC_IRQ_OSC_STABLE,
                      NFC_OSC_TIMEOUT_MS)) {
        return false;
    }
    bsp_nfc_probe_init_step = 5u;
    /* The datasheet requires reg_s to be toggled before Adjust Regulators. */
    if (!nfc_write(NFC_REG_REGULATOR, 0x80u) ||
        !nfc_write(NFC_REG_REGULATOR, 0x00u) ||
        !nfc_read(NFC_REG_IRQ_TIMER, &ignored) ||
        !nfc_command(NFC_CMD_ADJUST_REG) ||
        !nfc_wait_irq(NFC_REG_IRQ_TIMER, NFC_IRQ_CMD_DONE,
                      NFC_SPI_TIMEOUT_MS)) {
        return false;
    }
    bsp_nfc_probe_init_step = 6u;
    return true;
}

bool bsp_nfc_probe_amplitude(uint8_t *amplitude)
{
    uint8_t ignored = 0u;

    if (amplitude == NULL) {
        return false;
    }
    /* Reading the IRQ register clears any completion from an earlier sample. */
    if (!nfc_read(NFC_REG_IRQ_TIMER, &ignored) ||
        !nfc_command(NFC_CMD_MEASURE_AMPL) ||
        !nfc_wait_irq(NFC_REG_IRQ_TIMER, NFC_IRQ_CMD_DONE,
                      NFC_SPI_TIMEOUT_MS)) {
        return false;
    }
    return nfc_read(NFC_REG_ADC_OUTPUT, amplitude);
}

bool bsp_nfc_poll_uid(uint8_t uid[BSP_NFC_UID_MAX_BYTES], uint8_t *uid_length)
{
    static const uint8_t cascade_select[3] = { 0x93u, 0x95u, 0x97u };
    uint8_t atqa[2] = { 0u, 0u };
    uint8_t anticollision[5];
    uint8_t sak_frame[3];
    uint8_t response_length = 0u;
    uint8_t uid_used = 0u;
    uint8_t level;

    bsp_nfc_poll_step = 0u;
    bsp_nfc_poll_irq_main = 0u;
    bsp_nfc_poll_irq_error = 0u;
    bsp_nfc_poll_atqa[0] = 0u;
    bsp_nfc_poll_atqa[1] = 0u;
    if (uid == NULL || uid_length == NULL || !bsp_nfc_probe_chip_id) {
        bsp_nfc_poll_fail_code = 1u;
        return false;
    }
    *uid_length = 0u;

    if (!s_nfc_typea_ready) {
        /* ISO14443-A initiator (om[6:3] = 0001), OOK, 106 kbit/s each way.
         * om = 0000 would select NFCIP-1 active mode, which never receives a
         * passive card's load modulation. */
        if (!nfc_write(NFC_REG_MODE, 0x08u) ||
            !nfc_write(NFC_REG_BIT_RATE, 0x00u) ||
            !nfc_write(NFC_REG_ISO_A, 0x00u) ||
            !nfc_write(NFC_REG_OPERATION, 0xC8u)) {
            bsp_nfc_poll_fail_code = 2u;
            return false;
        }
        HAL_Delay(5u); /* ISO14443-A RF field-on guard time. */
        s_nfc_typea_ready = true;
    }
    bsp_nfc_poll_step = 1u;

    /* Drop the field long enough to reset a previously selected card to IDLE,
     * then restart it and allow the ISO14443-A guard time before REQA. */
    if (!nfc_command(NFC_CMD_STOP_ALL) ||
        !nfc_command(NFC_CMD_RESET_RX_GAIN) ||
        !nfc_write(NFC_REG_OPERATION, 0x80u)) {
        bsp_nfc_poll_fail_code = 3u;
        return false;
    }
    HAL_Delay(5u);
    if (!nfc_write(NFC_REG_OPERATION, 0xC8u)) {
        bsp_nfc_poll_fail_code = 4u;
        return false;
    }
    HAL_Delay(5u);
    /* REQA is a 7-bit short frame; the direct command generates its framing.
     * DS12484 Table 36: the chip skips the CRC check itself for the ATQA and
     * for anticollision frames (antcl = 1). */
    if (!nfc_write(NFC_REG_ISO_A, 0x00u) ||
        !nfc_write(NFC_REG_NUM_TX2, 0x07u)) {
        bsp_nfc_poll_fail_code = 5u;
        return false;
    }
    if (!nfc_command(NFC_CMD_TX_REQA)) {
        bsp_nfc_poll_fail_code = 6u;
        return false;
    }
    if (!nfc_wait_rx(NFC_RX_TIMEOUT_MS)) {
        bsp_nfc_poll_fail_code = 7u;
        return false;
    }
    uint16_t atqa_length = 0u;
    if (!nfc_fifo_length(&atqa_length) || atqa_length != sizeof(atqa) ||
        !nfc_fifo_read(atqa, sizeof(atqa))) {
        bsp_nfc_poll_fail_code = 8u;
        return false;
    }
    bsp_nfc_poll_atqa[0] = atqa[0];
    bsp_nfc_poll_atqa[1] = atqa[1];
    bsp_nfc_poll_fail_code = 0u;
    bsp_nfc_poll_step = 2u;

    /* Resolve the UID cascade levels. This single-card reader path rejects
     * anticollision collisions instead of returning a possibly mixed UID. */
    for (level = 0u; level < 3u; level++) {
        uint8_t anticoll_frame[2] = { cascade_select[level], 0x20u };
        if (!nfc_write(NFC_REG_ISO_A, 0x01u) ||
            !nfc_typea_transceive(anticoll_frame, sizeof(anticoll_frame),
                                  false, anticollision,
                                  sizeof(anticollision), &response_length) ||
            response_length != sizeof(anticollision)) {
            bsp_nfc_poll_fail_code = 9u;
            return false;
        }
        if ((uint8_t)(anticollision[0] ^ anticollision[1] ^ anticollision[2] ^
                      anticollision[3]) != anticollision[4]) {
            bsp_nfc_poll_fail_code = 10u;
            return false;
        }
        bsp_nfc_poll_step = 3u;

        uint8_t select_frame[7] = { cascade_select[level], 0x70u,
                                     anticollision[0], anticollision[1],
                                     anticollision[2], anticollision[3],
                                     anticollision[4] };
        if (!nfc_write(NFC_REG_ISO_A, 0x00u) ||
            !nfc_typea_transceive(select_frame, sizeof(select_frame), true,
                                  sak_frame, sizeof(sak_frame),
                                  &response_length) ||
            (response_length != 1u && response_length != 3u)) {
            bsp_nfc_poll_fail_code = 11u;
            return false;
        }
        if (response_length == 3u) {
            uint16_t crc = nfc_crc_a(sak_frame, 1u);
            if (sak_frame[1] != (uint8_t)crc ||
                sak_frame[2] != (uint8_t)(crc >> 8)) {
                bsp_nfc_poll_fail_code = 11u;
                return false;
            }
        }
        bsp_nfc_poll_step = 4u;

        bool has_cascade_tag = anticollision[0] == 0x88u;
        uint8_t copy_start = has_cascade_tag ? 1u : 0u;
        uint8_t copy_count = has_cascade_tag ? 3u : 4u;
        if ((uint16_t)uid_used + copy_count > BSP_NFC_UID_MAX_BYTES) {
            return false;
        }
        memcpy(&uid[uid_used], &anticollision[copy_start], copy_count);
        uid_used = (uint8_t)(uid_used + copy_count);

        bool more_levels = (sak_frame[0] & 0x04u) != 0u;
        if (!more_levels) {
            if (has_cascade_tag || uid_used == 0u) {
                return false;
            }
            *uid_length = uid_used;
            bsp_nfc_poll_fail_code = 0u;
            bsp_nfc_poll_step = 5u;
            return true;
        }
        if (!has_cascade_tag || level == 2u) {
            return false;
        }
    }
    return false;
}

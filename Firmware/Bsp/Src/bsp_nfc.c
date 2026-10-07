/**
 * @file    bsp_nfc.c
 * @brief   Level 1 (HAL) — ST25R3916 NFC reader on SPI1.
 *
 * Register-level driver for ISO14443-A reader mode at 106 kbit/s. It moves
 * frames and reports what the chip flagged; the protocol above it (REQA,
 * anticollision, SELECT, CRC checks) is Level 2's iso14443a.c. Section and
 * table numbers refer to the ST25R3916 datasheet, DS12484 Rev 8.
 *
 * Waiting for the chip
 * --------------------
 * Completion is signalled on the IRQ pin (PB1), and this driver polls that
 * pin rather than the interrupt status registers. §4.3.3 forbids SPI traffic
 * while a timed direct command (Adjust regulators, Measure amplitude) runs,
 * and keeping the bus quiet during reception keeps SPI clock edges away from
 * the receiver. Only the interrupts waited on are unmasked, so the pin rises
 * for those alone. The status registers are read once more at a timeout, so
 * a broken IRQ line makes the reader slow rather than dead, and is counted in
 * dbg_nfc_irq_pin_misses.
 *
 * Wake-up mode
 * ------------
 * Between cards Level 2 parks the chip in wake-up mode (§4.2.4): oscillator
 * off, and on its own RC timer it drives the antenna briefly and measures the
 * amplitude against a reference. Only then is EXTI1 unmasked, and only I_wam
 * reaches the pin. The interrupt is one shot: the handler masks the line and
 * posts APP_EVT_NFC_WAKE, because the pin stays high until the status
 * registers are read and SPI belongs to the main loop. Leaving wake-up mode
 * reads them, which drops the pin, and waits for the oscillator.
 */
#include "bsp.h"
#include "app_debug.h"
#include "app_events.h"
#include <string.h>

SPI_HandleTypeDef hbsp_spi;

/* ------------------------------------------------------------------------ */
/* Chip definitions                                                         */
/* ------------------------------------------------------------------------ */

/* Registers, space A (Table 17). */
#define REG_IO_CONF1        0x00u
#define REG_IO_CONF2        0x01u
#define REG_OP_CONTROL      0x02u
#define REG_MODE            0x03u
#define REG_BIT_RATE        0x04u
#define REG_ISO14443A       0x05u
#define REG_NRT1            0x10u
#define REG_NRT2            0x11u
#define REG_TIMER_EMV       0x12u
#define REG_MASK_MAIN       0x16u
#define REG_MASK_TIMER      0x17u
#define REG_MASK_ERROR      0x18u
#define REG_MASK_PT         0x19u
#define REG_IRQ_MAIN        0x1Au   /* timer (1Bh) and error (1Ch) follow */
#define REG_FIFO_STATUS1    0x1Eu   /* FIFO status 2 (1Fh) follows */
#define REG_NUM_TX1         0x22u
#define REG_NUM_TX2         0x23u
#define REG_ADC_OUTPUT      0x25u
#define REG_REGULATOR       0x2Cu
#define REG_AUX_DISPLAY     0x31u
#define REG_WAKEUP_TIMER    0x32u
#define REG_AM_CONF         0x33u   /* amplitude measurement configuration */
#define REG_AM_REF          0x34u   /* amplitude measurement reference */
#define REG_AM_DISPLAY      0x36u   /* the wake-up mode's last amplitude reading */
#define REG_IC_IDENTITY     0x3Fu

/* SPI mode bytes (Table 11). */
#define SPI_REG_READ        0x40u
#define SPI_FIFO_LOAD       0x80u
#define SPI_FIFO_READ       0x9Fu

/* Direct commands (Table 13). */
#define CMD_SET_DEFAULT     0xC1u
#define CMD_STOP_ALL        0xC2u
#define CMD_TX_WITH_CRC     0xC4u
#define CMD_TX_WITHOUT_CRC  0xC5u
#define CMD_TX_REQA         0xC6u
#define CMD_MEASURE_AMPL    0xD3u
#define CMD_RESET_RX_GAIN   0xD5u
#define CMD_ADJUST_REG      0xD6u
#define CMD_TEST_ACCESS     0xFCu

/* Register values. */
#define OP_EN               0x80u   /* Ready mode: oscillator, regulators */
#define OP_RX_EN            0x40u
#define OP_TX_EN            0x08u
#define OP_WU               0x04u   /* wake-up mode (with en = 0) */
#define AUX_OSC_OK          0x10u
#define WUT_WUR_10MS        0x80u   /* wut counts 10 ms steps, else 100 ms */
#define WUT_SHIFT           4u
#define WUT_WAM             0x04u   /* amplitude measurement at each timeout */
#define AM_D_SHIFT          4u      /* am_ae = 0: compare with REG_AM_REF */
#define MODE_ISO14443A      0x08u   /* initiator, om = 0001, OOK (Tables 22, 23) */
#define ISOA_ANTCL          0x01u
#define IO_CONF1_NO_MCU_CLK 0x07u   /* MCU_CLK and its 32 kHz clock off */
#define IO_CONF2_SUP3V      0x80u
#define REGULATOR_REG_S     0x80u
#define IC_TYPE_MASK        0xF8u
#define IC_TYPE_ST25R3916   0x28u   /* ic_type 00101 (Table 117) */
#define FIFO2_B9_B8         0xC0u
#define FIFO2_UNF_OVR       0x30u
#define FIFO2_PARTIAL_BYTE  0x0Fu   /* fifo_lb, np_lb */

/* Interrupt bits, packed as dbg_nfc_last_irq. */
#define IRQ_OSC             0x000080u
#define IRQ_RXE             0x000010u
#define IRQ_COL             0x000004u
#define IRQ_DCT             0x008000u
#define IRQ_NRE             0x004000u
#define IRQ_CRC             0x800000u
#define IRQ_PAR             0x400000u
#define IRQ_ERR1            0x100000u   /* hard framing; soft framing (err2)
                                           leaves the data intact */
#define IRQ_WAM             0x040000u   /* wake-up amplitude measurement */
#define IRQ_RX_ERRORS       (IRQ_CRC | IRQ_PAR | IRQ_ERR1)
#define IRQ_RX_DONE         (IRQ_RXE | IRQ_NRE | IRQ_COL | IRQ_RX_ERRORS)
#define IRQ_WANTED          (IRQ_OSC | IRQ_DCT | IRQ_RX_DONE)

/* Mask registers: a 1 masks the source from the pin (Tables 58-61). RFU bits
 * are kept at 0. */
#define MASK_MAIN   ((uint8_t)(0xFEu & ~(IRQ_WANTED & 0xFFu)))
#define MASK_TIMER  ((uint8_t)(0xFFu & ~((IRQ_WANTED >> 8) & 0xFFu)))
#define MASK_ERROR  ((uint8_t)(0xFFu & ~((IRQ_WANTED >> 16) & 0xFFu)))
#define MASK_PT     0xFBu

/* Wake-up mode: I_wam alone. The chip starts its oscillator for every
 * measurement, so an unmasked I_osc (or anything else) would raise the pin
 * every period and wake the MCU for nothing. RFAL masks the same way. */
#define MASK_MAIN_WU   0xFEu
#define MASK_TIMER_WU  0xFFu
#define MASK_ERROR_WU  ((uint8_t)(0xFFu & ~((IRQ_WAM >> 16) & 0xFFu)))

#define MAX_FRAME   32u

/** Interrupt bits read from the chip and not yet consumed. */
static uint32_t s_irq;

/** The chip is in wake-up mode: its oscillator is off. */
static bool s_wakeup;

/* ------------------------------------------------------------------------ */
/* SPI                                                                      */
/* ------------------------------------------------------------------------ */

static bool spi_init(void)
{
    /* Mode 1, MSB first, software BSS: the ST25R3916 needs BSS held low for
     * a whole multi-byte frame, which the SPI's own NSS cannot do. */
    hbsp_spi.Instance = BSP_NFC_SPI;
    hbsp_spi.Init.Mode = SPI_MODE_MASTER;
    hbsp_spi.Init.Direction = SPI_DIRECTION_2LINES;
    hbsp_spi.Init.DataSize = SPI_DATASIZE_8BIT;
    hbsp_spi.Init.CLKPolarity = SPI_POLARITY_LOW;
    hbsp_spi.Init.CLKPhase = SPI_PHASE_2EDGE;
    hbsp_spi.Init.NSS = SPI_NSS_SOFT;
    hbsp_spi.Init.BaudRatePrescaler = BSP_NFC_SPI_PRESCALER;
    hbsp_spi.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hbsp_spi.Init.TIMode = SPI_TIMODE_DISABLE;
    hbsp_spi.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    hbsp_spi.Init.CRCPolynomial = 7u;
    hbsp_spi.Init.CRCLength = SPI_CRC_LENGTH_DATASIZE;
    hbsp_spi.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
    return HAL_SPI_Init(&hbsp_spi) == HAL_OK;
}

/** One BSS-framed transfer. @p rx may be NULL for a write. */
static bool spi_xfer(const uint8_t *tx, uint8_t *rx, uint16_t len)
{
    HAL_StatusTypeDef st;

    HAL_GPIO_WritePin(PORT_NFC_NSS, PIN_NFC_NSS, GPIO_PIN_RESET);
    if (rx != NULL) {
        st = HAL_SPI_TransmitReceive(&hbsp_spi, (uint8_t *)tx, rx, len,
                                     BSP_NFC_SPI_TIMEOUT_MS);
    } else {
        st = HAL_SPI_Transmit(&hbsp_spi, (uint8_t *)tx, len,
                              BSP_NFC_SPI_TIMEOUT_MS);
    }
    HAL_GPIO_WritePin(PORT_NFC_NSS, PIN_NFC_NSS, GPIO_PIN_SET);
    return st == HAL_OK;
}

static bool reg_write(uint8_t reg, uint8_t value)
{
    uint8_t tx[2] = { reg, value };
    return spi_xfer(tx, NULL, 2u);
}

/** Read up to three consecutive registers (address auto-increments). */
static bool reg_read(uint8_t reg, uint8_t *values, uint8_t n)
{
    uint8_t tx[4] = { (uint8_t)(SPI_REG_READ | reg), 0u, 0u, 0u };
    uint8_t rx[4] = { 0u, 0u, 0u, 0u };

    if (n == 0u || n > 3u || !spi_xfer(tx, rx, (uint16_t)(n + 1u))) {
        return false;
    }
    memcpy(values, &rx[1], n);
    return true;
}

static bool command(uint8_t cmd)
{
    return spi_xfer(&cmd, NULL, 1u);
}

static bool fifo_read(uint8_t *bytes, uint16_t n)
{
    uint8_t tx[1u + MAX_FRAME];
    uint8_t rx[1u + MAX_FRAME];

    if (n == 0u || n > MAX_FRAME) {
        return false;
    }
    memset(tx, 0, sizeof(tx));
    tx[0] = SPI_FIFO_READ;
    if (!spi_xfer(tx, rx, (uint16_t)(n + 1u))) {
        return false;
    }
    memcpy(bytes, &rx[1], n);
    return true;
}

/* ------------------------------------------------------------------------ */
/* Interrupts                                                               */
/* ------------------------------------------------------------------------ */

/** Read (and so clear) the three interrupt status registers into s_irq. */
static bool irq_read(void)
{
    uint8_t r[3];

    if (!reg_read(REG_IRQ_MAIN, r, 3u)) {
        return false;
    }
    s_irq |= (uint32_t)r[0] | ((uint32_t)r[1] << 8) | ((uint32_t)r[2] << 16);
    return true;
}

static void irq_clear(void)
{
    (void)irq_read();
    s_irq = 0u;
}

/**
 * Wait until one of @p mask is pending, polling the IRQ pin. Returns every
 * bit collected so far; zero bits of @p mask means the wait timed out.
 */
static uint32_t irq_wait(uint32_t mask, uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();

    while ((s_irq & mask) == 0u) {
        /* Strictly greater, so a wait never ends early on a tick boundary. */
        bool expired = (uint32_t)(HAL_GetTick() - start) > timeout_ms;
        bool pin = HAL_GPIO_ReadPin(PORT_NFC_IRQ, PIN_NFC_IRQ) == NFC_IRQ_ACTIVE_LEVEL;

        if (!pin && !expired) {
            continue;
        }
        if (!irq_read()) {
            break;
        }
        if ((s_irq & mask) != 0u) {
            if (!pin) {
                dbg_nfc_irq_pin_misses++;
            }
            break;
        }
        if (expired) {
            break;
        }
    }
    return s_irq;
}

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */

/** The interrupt masks for normal work, or for wake-up mode. */
static bool irq_masks(bool wakeup)
{
    return reg_write(REG_MASK_MAIN, wakeup ? MASK_MAIN_WU : MASK_MAIN) &&
           reg_write(REG_MASK_TIMER, wakeup ? MASK_TIMER_WU : MASK_TIMER) &&
           reg_write(REG_MASK_ERROR, wakeup ? MASK_ERROR_WU : MASK_ERROR);
}

static void exti_mask(void)
{
    CLEAR_BIT(EXTI->IMR1, PIN_NFC_IRQ);
    __HAL_GPIO_EXTI_CLEAR_IT(PIN_NFC_IRQ);
}

void bsp_nfc_wake_irq(void)
{
    /* One shot: the pin stays high until the main loop reads the status. */
    CLEAR_BIT(EXTI->IMR1, PIN_NFC_IRQ);
    app_event_post(APP_EVT_NFC_WAKE);
}

/** §4.4.10: toggle reg_s, then let the chip set VDD_RF 250 mV below VDD_TX. */
static bool adjust_regulators(void)
{
    if (!reg_write(REG_REGULATOR, REGULATOR_REG_S) ||
        !reg_write(REG_REGULATOR, 0x00u)) {
        return false;
    }
    irq_clear();
    if (!command(CMD_ADJUST_REG)) {
        return false;
    }
    return (irq_wait(IRQ_DCT, BSP_NFC_ADJUST_TIMEOUT_MS) & IRQ_DCT) != 0u;
}

/** §4.2.13: stop anything running, reset the AGC, pick the frame type. Stop
 *  all activities also clears the FIFO and the interrupt status. */
static bool exchange_prepare(uint8_t iso14443a)
{
    if (!command(CMD_STOP_ALL) || !command(CMD_RESET_RX_GAIN) ||
        !reg_write(REG_ISO14443A, iso14443a)) {
        return false;
    }
    s_irq = 0u;
    return true;
}

/** Wait for the response to the frame just sent and collect it. */
static plat_nfc_status_t exchange_finish(uint8_t *rx, uint8_t rx_cap, uint8_t *rx_len)
{
    uint32_t irq = irq_wait(IRQ_RX_DONE, BSP_NFC_RX_TIMEOUT_MS);
    uint8_t fifo[2];
    uint16_t n;

    dbg_nfc_last_irq = irq;
    *rx_len = 0u;

    if ((irq & IRQ_COL) != 0u) {
        return PLAT_NFC_COLLISION;
    }
    if ((irq & IRQ_RX_ERRORS) != 0u) {
        return PLAT_NFC_RX_ERROR;
    }
    if ((irq & IRQ_RXE) == 0u) {
        return PLAT_NFC_TIMEOUT;   /* no-response timer, or no IRQ at all */
    }

    if (!reg_read(REG_FIFO_STATUS1, fifo, 2u)) {
        return PLAT_NFC_IO_ERROR;
    }
    n = (uint16_t)(fifo[0] | ((uint16_t)(fifo[1] & FIFO2_B9_B8) << 2));
    if ((fifo[1] & (FIFO2_UNF_OVR | FIFO2_PARTIAL_BYTE)) != 0u ||
        n == 0u || n > rx_cap) {
        return PLAT_NFC_RX_ERROR;
    }
    if (!fifo_read(rx, n)) {
        return PLAT_NFC_IO_ERROR;
    }
    *rx_len = (uint8_t)n;
    return PLAT_NFC_OK;
}

/* ------------------------------------------------------------------------ */
/* platform_if                                                              */
/* ------------------------------------------------------------------------ */

bool plat_nfc_init(bool supply_3v3, uint8_t *chip_id)
{
    static const uint8_t overheat_fix[3] = { CMD_TEST_ACCESS, 0x04u, 0x10u };
    uint8_t id = 0u;

    *chip_id = 0u;
    exti_mask();
    s_wakeup = false;
    if (!spi_init() || !reg_read(REG_IC_IDENTITY, &id, 1u)) {
        return false;
    }
    *chip_id = id;
    if ((id & IC_TYPE_MASK) != IC_TYPE_ST25R3916) {
        return false;
    }

    /* §4.1: Set default, then the 3-byte frame that stops the overheat
     * protection tripping early; both are required after every power-on. */
    if (!command(CMD_SET_DEFAULT) || !spi_xfer(overheat_fix, NULL, 3u)) {
        return false;
    }

    if (!reg_write(REG_IO_CONF1, IO_CONF1_NO_MCU_CLK) ||
        !reg_write(REG_IO_CONF2, supply_3v3 ? IO_CONF2_SUP3V : 0x00u) ||
        !irq_masks(false) ||
        !reg_write(REG_MASK_PT, MASK_PT)) {
        return false;
    }

    /* Ready mode; the oscillator reports when it is stable. */
    irq_clear();
    if (!reg_write(REG_OP_CONTROL, OP_EN) ||
        (irq_wait(IRQ_OSC, BSP_NFC_OSC_TIMEOUT_MS) & IRQ_OSC) == 0u) {
        return false;
    }

    if (!adjust_regulators()) {
        return false;
    }

    /* The mode register only takes writes once the oscillator is stable
     * (Table 22, note 1). No-response timer in 64/fc steps, started by the
     * chip at the end of every transmission. */
    return reg_write(REG_MODE, MODE_ISO14443A) &&
           reg_write(REG_BIT_RATE, 0x00u) &&
           reg_write(REG_ISO14443A, 0x00u) &&
           reg_write(REG_TIMER_EMV, 0x00u) &&
           reg_write(REG_NRT1, (uint8_t)(BSP_NFC_NRT_64FC >> 8)) &&
           reg_write(REG_NRT2, (uint8_t)(BSP_NFC_NRT_64FC & 0xFFu));
}

bool plat_nfc_set_supply(bool supply_3v3)
{
    return reg_write(REG_IO_CONF2, supply_3v3 ? IO_CONF2_SUP3V : 0x00u) &&
           adjust_regulators();
}

void plat_nfc_field(bool on)
{
    (void)reg_write(REG_OP_CONTROL, on ? (uint8_t)(OP_EN | OP_RX_EN | OP_TX_EN)
                                       : OP_EN);
}

plat_nfc_status_t plat_nfc_reqa(uint8_t atqa[2])
{
    uint8_t n = 0u;
    plat_nfc_status_t st;

    /* Transmit REQA frames itself and turns the CRC check off for the ATQA
     * (§4.4.4); antcl must be 0 for it. */
    if (!exchange_prepare(0x00u) || !command(CMD_TX_REQA)) {
        return PLAT_NFC_IO_ERROR;
    }
    st = exchange_finish(atqa, 2u, &n);
    if (st == PLAT_NFC_OK && n != 2u) {
        st = PLAT_NFC_RX_ERROR;
    }
    return st;
}

plat_nfc_status_t plat_nfc_transceive(const uint8_t *tx, uint8_t tx_len,
                                      uint8_t flags, uint8_t *rx,
                                      uint8_t rx_cap, uint8_t *rx_len)
{
    uint8_t frame[1u + MAX_FRAME];

    *rx_len = 0u;
    if (tx == NULL || tx_len == 0u || tx_len > MAX_FRAME) {
        return PLAT_NFC_IO_ERROR;
    }

    /* Tables 70-71: ntx<12:0> whole bytes, nbtx = 0 (no partial byte). */
    frame[0] = SPI_FIFO_LOAD;
    memcpy(&frame[1], tx, tx_len);
    if (!exchange_prepare((flags & PLAT_NFC_ANTICOLLISION) != 0u ? ISOA_ANTCL : 0x00u) ||
        !reg_write(REG_NUM_TX1, (uint8_t)(tx_len >> 5)) ||
        !reg_write(REG_NUM_TX2, (uint8_t)((tx_len & 0x1Fu) << 3)) ||
        !spi_xfer(frame, NULL, (uint16_t)(tx_len + 1u)) ||
        !command((flags & PLAT_NFC_TX_CRC) != 0u ? CMD_TX_WITH_CRC
                                                 : CMD_TX_WITHOUT_CRC)) {
        return PLAT_NFC_IO_ERROR;
    }
    return exchange_finish(rx, rx_cap, rx_len);
}

bool plat_nfc_measure_amplitude(uint8_t *raw)
{
    /* §4.4.8: drives the field briefly itself; 25 us at most. */
    irq_clear();
    if (!command(CMD_MEASURE_AMPL) ||
        (irq_wait(IRQ_DCT, BSP_NFC_MEASURE_TIMEOUT_MS) & IRQ_DCT) == 0u) {
        return false;
    }
    return reg_read(REG_ADC_OUTPUT, raw, 1u);
}

bool plat_nfc_wakeup_arm(uint8_t reference, uint8_t delta, uint16_t period_ms)
{
    uint8_t wut;
    uint16_t steps;

    /* Wake-up timer control: wur picks 10-80 ms or 100-800 ms, wut the step
     * count minus one. Take the longest period not over the one asked for. */
    if (period_ms < 100u) {
        steps = (uint16_t)(period_ms / 10u);
        steps = (steps == 0u) ? 1u : steps;
        wut = (uint8_t)(WUT_WUR_10MS | ((steps - 1u) << WUT_SHIFT));
    } else {
        steps = (uint16_t)(period_ms / 100u);
        steps = (steps > 8u) ? 8u : steps;
        wut = (uint8_t)((steps - 1u) << WUT_SHIFT);
    }

    exti_mask();
    if (!command(CMD_STOP_ALL) ||
        !reg_write(REG_OP_CONTROL, OP_EN) ||
        !reg_write(REG_AM_REF, reference) ||
        !reg_write(REG_AM_CONF, (uint8_t)((delta & 0x0Fu) << AM_D_SHIFT)) ||
        !reg_write(REG_WAKEUP_TIMER, (uint8_t)(wut | WUT_WAM)) ||
        !irq_masks(true)) {
        (void)irq_masks(false);
        return false;
    }
    irq_clear();
    if (!reg_write(REG_OP_CONTROL, OP_WU)) {
        (void)reg_write(REG_OP_CONTROL, OP_EN);
        (void)irq_masks(false);
        return false;
    }
    s_wakeup = true;

    SET_BIT(EXTI->IMR1, PIN_NFC_IRQ);
    /* An edge before the unmask would be lost, but the pin holds its level
     * until the status is read, so a level check catches it. */
    if (HAL_GPIO_ReadPin(PORT_NFC_IRQ, PIN_NFC_IRQ) == NFC_IRQ_ACTIVE_LEVEL) {
        __HAL_GPIO_EXTI_GENERATE_SWIT(PIN_NFC_IRQ);
    }
    return true;
}

bool plat_nfc_wakeup_disarm(uint8_t *last_raw)
{
    uint8_t aux = 0u;

    *last_raw = 0u;
    exti_mask();
    if (!s_wakeup) {
        return true;
    }
    s_wakeup = false;

    /* What the chip measured last in wake-up mode: Level 2 learns from it how
     * that measurement relates to the Measure amplitude command. */
    (void)reg_read(REG_AM_DISPLAY, last_raw, 1u);

    /* Reading the status drops the pin; keep what woke us for Live
     * Expressions (I_wam is 0x040000). */
    s_irq = 0u;
    (void)irq_read();
    dbg_nfc_last_irq = s_irq;
    dbg_nfc_wake_irq = s_irq;
    s_irq = 0u;

    /* Normal masks back before the oscillator starts: I_osc is waited on. */
    if (!irq_masks(false) || !reg_write(REG_OP_CONTROL, OP_EN)) {
        return false;
    }
    /* The oscillator may already be up, part-way through one of the chip's
     * own measurements, and then I_osc need not come. osc_ok is the truth. */
    if (!reg_read(REG_AUX_DISPLAY, &aux, 1u)) {
        return false;
    }
    if ((aux & AUX_OSC_OK) == 0u) {
        (void)irq_wait(IRQ_OSC, BSP_NFC_OSC_TIMEOUT_MS);
        if (!reg_read(REG_AUX_DISPLAY, &aux, 1u)) {
            return false;
        }
    }
    s_irq = 0u;
    return (aux & AUX_OSC_OK) != 0u;
}

void plat_nfc_power_down(void)
{
    /* The reader runs straight off the cell, so it has to be told to sleep
     * or it keeps its oscillator running while the unit is "off". */
    exti_mask();
    s_wakeup = false;
    if (hbsp_spi.State == HAL_SPI_STATE_RESET && !spi_init()) {
        return;
    }
    (void)command(CMD_STOP_ALL);
    (void)reg_write(REG_OP_CONTROL, 0x00u);
}

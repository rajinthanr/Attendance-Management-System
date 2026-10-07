/**
 * @file    bsp_usb.c
 * @brief   Level 1 (HAL) — USB mass storage device.
 *
 * The SCSI callbacks below forward straight into usb_storage.c. Every decision
 * about what the host sees -- the volume size, the FAT image, the CSV rows --
 * is made up in Level 2; this file only moves blocks and reports status.
 */
#include "bsp.h"
#include "usbd_core.h"
#include "usbd_msc.h"
#include "usbd_msc_scsi.h"
#include "usbd_desc.h"
#include "usb_storage.h"
#include "app_events.h"

static USBD_HandleTypeDef s_usbd;
static bool s_started;
static volatile bool s_ejected;

/* ------------------------------------------------------------------------ */
/* Eject detection                                                          */
/* ------------------------------------------------------------------------ */

/* ST's MSC class handles START STOP UNIT itself and tells nobody, so the
 * class is registered through a copy whose DataOut looks at the command block
 * the original has just decoded. A START = 0 the class accepted (the medium was
 * not locked by PREVENT ALLOW MEDIUM REMOVAL) is the host's eject. */
static USBD_ClassTypeDef s_msc_class;

static uint8_t msc_data_out(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
    uint8_t ret = USBD_MSC.DataOut(pdev, epnum);
    const USBD_MSC_BOT_HandleTypeDef *hmsc =
        (const USBD_MSC_BOT_HandleTypeDef *)pdev->pClassDataCmsit[pdev->classId];

    if (hmsc != NULL && hmsc->cbw.CB[0] == SCSI_START_STOP_UNIT &&
        (hmsc->cbw.CB[4] & 0x01u) == 0u && hmsc->scsi_medium_state != SCSI_MEDIUM_LOCKED) {
        s_ejected = true;
    }
    return ret;
}

/* ------------------------------------------------------------------------ */
/* SCSI storage callbacks                                                   */
/* ------------------------------------------------------------------------ */

/* Standard INQUIRY response: 8 bytes of vendor, 16 of product, 4 of revision.
 * The field widths are fixed by the SCSI spec, hence the padding. */
static int8_t s_inquiry[] = {
    0x00,                    /* direct access block device */
    0x80,                    /* removable media            */
    0x02,                    /* SPC-2                      */
    0x02,                    /* response data format       */
    (STANDARD_INQUIRY_DATA_LEN - 5),
    0x00, 0x00, 0x00,
    'C', 'A', 'S', ' ', ' ', ' ', ' ', ' ',                      /* vendor  */
    'A', 't', 't', 'e', 'n', 'd', 'a', 'n',                      /* product */
    'c', 'e', ' ', 'L', 'o', 'g', ' ', ' ',
    '1', '.', '0', '0'                                           /* revision*/
};

static int8_t storage_init(uint8_t lun)
{
    (void)lun;
    return 0;
}

static int8_t storage_get_capacity(uint8_t lun, uint32_t *block_num, uint16_t *block_size)
{
    (void)lun;
    *block_num = usbs_sector_count();
    *block_size = usbs_sector_size();
    return 0;
}

static int8_t storage_is_ready(uint8_t lun)
{
    (void)lun;
    return 0;
}

static int8_t storage_is_write_protected(uint8_t lun)
{
    (void)lun;
    /* SETTINGS.CSV is edited by the host, so the volume has to be writable.
     * Writes only ever land in RAM; usbs_end() decides what reaches flash. */
    return 0;
}

static int8_t storage_read(uint8_t lun, uint8_t *buf, uint32_t blk_addr, uint16_t blk_len)
{
    (void)lun;

    if (!usbs_read(blk_addr, buf, blk_len)) {
        return -1;
    }

    /* Tells the application the host is actually pulling the file, which is
     * the flow chart's "User opens/copies file?" branch. */
    app_event_post(APP_EVT_USB_ACTIVITY);
    return 0;
}

static int8_t storage_write(uint8_t lun, uint8_t *buf, uint32_t blk_addr, uint16_t blk_len)
{
    (void)lun;

    /* usbs_write() refuses sectors that belong to the read-only files; the
     * MSC layer turns -1 into a write-error sense code for the host. */
    return usbs_write(blk_addr, buf, blk_len) ? 0 : -1;
}

static int8_t storage_get_max_lun(void)
{
    return 0;   /* a single logical unit */
}

static USBD_StorageTypeDef s_storage_fops = {
    storage_init,
    storage_get_capacity,
    storage_is_ready,
    storage_is_write_protected,
    storage_read,
    storage_write,
    storage_get_max_lun,
    s_inquiry
};

/* ------------------------------------------------------------------------ */
/* platform_if                                                              */
/* ------------------------------------------------------------------------ */

void plat_usb_start(void)
{
    if (s_started) {
        return;
    }

    /* USB needs both a 48 MHz source and a core fast enough to keep up. */
    bsp_clock_set_usb_speed(true);

    if (USBD_Init(&s_usbd, &bsp_usb_descriptors, 0u) != USBD_OK) {
        return;
    }
    s_msc_class = USBD_MSC;
    s_msc_class.DataOut = msc_data_out;
    s_ejected = false;
    if (USBD_RegisterClass(&s_usbd, &s_msc_class) != USBD_OK) {
        return;
    }
    if (USBD_MSC_RegisterStorage(&s_usbd, &s_storage_fops) != USBD_OK) {
        return;
    }
    if (USBD_Start(&s_usbd) != USBD_OK) {
        return;
    }

    s_started = true;
}

void plat_usb_stop(void)
{
    if (!s_started) {
        return;
    }

    (void)USBD_Stop(&s_usbd);
    (void)USBD_DeInit(&s_usbd);

    bsp_clock_set_usb_speed(false);
    s_started = false;
}

bool plat_usb_configured(void)
{
    return s_started && (s_usbd.dev_state == USBD_STATE_CONFIGURED);
}

bool plat_usb_ejected(void)
{
    return s_started && s_ejected;
}

/* ------------------------------------------------------------------------ */
/* MSP: the USB peripheral's clocks, pins and interrupt                     */
/* ------------------------------------------------------------------------ */

void HAL_PCD_MspInit(PCD_HandleTypeDef *h)
{
    if (h->Instance != USB) {
        return;
    }

    GPIO_InitTypeDef g = { 0 };

    __HAL_RCC_GPIOA_CLK_ENABLE();

    g.Pin = PIN_USB_DM | PIN_USB_DP;
    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF10_USB_FS;
    HAL_GPIO_Init(PORT_USB, &g);

    __HAL_RCC_USB_CLK_ENABLE();

    /* VDDUSB is a separate supply domain on this part and comes up isolated.
     * Without this the transceiver stays dead and the host sees nothing. */
    HAL_PWREx_EnableVddUSB();

    /* The clock recovery system disciplines HSI48 against the host's
     * start-of-frame, which is what removes the need for a USB crystal. */
    __HAL_RCC_CRS_CLK_ENABLE();
    {
        RCC_CRSInitTypeDef crs = { 0 };

        crs.Prescaler = RCC_CRS_SYNC_DIV1;
        crs.Source = RCC_CRS_SYNC_SOURCE_USB;
        crs.Polarity = RCC_CRS_SYNC_POLARITY_RISING;
        crs.ReloadValue = __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000u, 1000u);
        crs.ErrorLimitValue = 34u;
        crs.HSI48CalibrationValue = RCC_CRS_HSI48CALIBRATION_DEFAULT;
        HAL_RCCEx_CRSConfig(&crs);
    }

    HAL_NVIC_SetPriority(USB_IRQn, BSP_PRIO_USB, 0u);
    HAL_NVIC_EnableIRQ(USB_IRQn);
}

void HAL_PCD_MspDeInit(PCD_HandleTypeDef *h)
{
    if (h->Instance != USB) {
        return;
    }

    HAL_NVIC_DisableIRQ(USB_IRQn);

    __HAL_RCC_USB_CLK_DISABLE();
    __HAL_RCC_CRS_CLK_DISABLE();
    HAL_PWREx_DisableVddUSB();

    /* Back to analog: a floating AF pin on an unpowered bus leaks. */
    GPIO_InitTypeDef g = { 0 };
    g.Pin = PIN_USB_DM | PIN_USB_DP;
    g.Mode = GPIO_MODE_ANALOG;
    g.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(PORT_USB, &g);
}

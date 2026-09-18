/**
 * @file    usbd_desc.c
 * @brief   Level 1 (HAL) — USB descriptors.
 *
 * The VID/PID pair below is ST's own, which is what CubeMX emits and what a
 * development board is expected to use. A shipping product needs its own
 * assignment; both values are collected here so that is a one-line change.
 */
#include "usbd_core.h"
#include "usbd_desc.h"
#include "bsp.h"

#define USBD_VID              0x0483u          /* STMicroelectronics */
#define USBD_PID              0x5720u          /* ST mass storage    */
#define USBD_LANGID_STRING    0x0409u          /* en-US              */
#define USBD_MANUFACTURER     "Card Attendance System"
#define USBD_PRODUCT          "Attendance Reader"
#define USBD_CONFIGURATION    "MSC Config"
#define USBD_INTERFACE        "MSC Interface"

static uint8_t *desc_device(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *desc_langid(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *desc_manufacturer(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *desc_product(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *desc_serial(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *desc_configuration(USBD_SpeedTypeDef speed, uint16_t *length);
static uint8_t *desc_interface(USBD_SpeedTypeDef speed, uint16_t *length);

USBD_DescriptorsTypeDef bsp_usb_descriptors = {
    desc_device,
    desc_langid,
    desc_manufacturer,
    desc_product,
    desc_serial,
    desc_configuration,
    desc_interface
};

__ALIGN_BEGIN static uint8_t s_device_desc[USB_LEN_DEV_DESC] __ALIGN_END = {
    USB_LEN_DEV_DESC,           /* bLength            */
    USB_DESC_TYPE_DEVICE,       /* bDescriptorType    */
    0x00u, 0x02u,               /* bcdUSB 2.00        */
    0x00u,                      /* bDeviceClass: per interface */
    0x00u,                      /* bDeviceSubClass    */
    0x00u,                      /* bDeviceProtocol    */
    USB_MAX_EP0_SIZE,           /* bMaxPacketSize0    */
    LOBYTE(USBD_VID), HIBYTE(USBD_VID),
    LOBYTE(USBD_PID), HIBYTE(USBD_PID),
    0x00u, 0x02u,               /* bcdDevice 2.00     */
    USBD_IDX_MFC_STR,
    USBD_IDX_PRODUCT_STR,
    USBD_IDX_SERIAL_STR,
    USBD_MAX_NUM_CONFIGURATION
};

__ALIGN_BEGIN static uint8_t s_langid_desc[USB_LEN_LANGID_STR_DESC] __ALIGN_END = {
    USB_LEN_LANGID_STR_DESC,
    USB_DESC_TYPE_STRING,
    LOBYTE(USBD_LANGID_STRING), HIBYTE(USBD_LANGID_STRING)
};

__ALIGN_BEGIN static uint8_t s_str_desc[USBD_MAX_STR_DESC_SIZ] __ALIGN_END;

/** Serial number as 24 hex digits, built from the 96-bit unique device ID. */
__ALIGN_BEGIN static uint8_t s_serial_desc[26] __ALIGN_END = {
    26u,
    USB_DESC_TYPE_STRING
};

static void hex_to_unicode(uint32_t value, uint8_t *out, uint8_t digits)
{
    uint8_t i;

    for (i = 0u; i < digits; i++) {
        uint8_t nibble = (uint8_t)((value >> ((digits - 1u - i) * 4u)) & 0x0Fu);

        /* UTF-16LE: ASCII byte then a zero high byte. */
        out[i * 2u] = (nibble < 10u) ? (uint8_t)('0' + nibble)
                                     : (uint8_t)('A' + (nibble - 10u));
        out[(i * 2u) + 1u] = 0u;
    }
}

static void build_serial(void)
{
    /* Folding the three words into two keeps the descriptor at the 24-digit
     * length hosts expect for MSC while staying unique per part. */
    uint32_t a = HAL_GetUIDw0() + HAL_GetUIDw2();
    uint32_t b = HAL_GetUIDw1();

    hex_to_unicode(a, &s_serial_desc[2], 8u);
    hex_to_unicode(b, &s_serial_desc[18], 4u);
}

static uint8_t *desc_device(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    *length = (uint16_t)sizeof(s_device_desc);
    return s_device_desc;
}

static uint8_t *desc_langid(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    *length = (uint16_t)sizeof(s_langid_desc);
    return s_langid_desc;
}

static uint8_t *desc_manufacturer(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    USBD_GetString((uint8_t *)USBD_MANUFACTURER, s_str_desc, length);
    return s_str_desc;
}

static uint8_t *desc_product(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    USBD_GetString((uint8_t *)USBD_PRODUCT, s_str_desc, length);
    return s_str_desc;
}

static uint8_t *desc_serial(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    build_serial();
    *length = (uint16_t)sizeof(s_serial_desc);
    return s_serial_desc;
}

static uint8_t *desc_configuration(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    USBD_GetString((uint8_t *)USBD_CONFIGURATION, s_str_desc, length);
    return s_str_desc;
}

static uint8_t *desc_interface(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    USBD_GetString((uint8_t *)USBD_INTERFACE, s_str_desc, length);
    return s_str_desc;
}

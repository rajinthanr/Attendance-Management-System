/**
 * @file    usbd_conf.h
 * @brief   Level 1 (HAL) — configuration for ST's USB device library.
 *
 * Sized for exactly one MSC interface and static allocation: there is no heap
 * in this firmware, and a device that hands out memory on enumeration is a
 * device that can fail on the tenth plug-in.
 */
#ifndef USBD_CONF_H
#define USBD_CONF_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stm32l4xx_hal.h"

#define USBD_MAX_NUM_INTERFACES        1U
#define USBD_MAX_NUM_CONFIGURATION     1U
#define USBD_MAX_STR_DESC_SIZ          512U
#define USBD_SELF_POWERED              0U    /* bus powered while attached */
#define USBD_DEBUG_LEVEL               0U
#define MSC_MEDIA_PACKET               512U
#define USBD_SUPPORT_USER_STRING_DESC  0U

/* Static allocation: the class handle is a single fixed object. */
void *USBD_static_malloc(uint32_t size);
void  USBD_static_free(void *p);

#define USBD_malloc         USBD_static_malloc
#define USBD_free           USBD_static_free
#define USBD_memset         memset
#define USBD_memcpy         memcpy
#define USBD_Delay          HAL_Delay

#if (USBD_DEBUG_LEVEL > 0U)
#define USBD_UsrLog(...)    do { } while (0)
#define USBD_ErrLog(...)    do { } while (0)
#define USBD_DbgLog(...)    do { } while (0)
#else
#define USBD_UsrLog(...)    do { } while (0)
#define USBD_ErrLog(...)    do { } while (0)
#define USBD_DbgLog(...)    do { } while (0)
#endif

#endif /* USBD_CONF_H */

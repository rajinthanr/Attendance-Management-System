/**
 * @file    usbd_conf.c
 * @brief   Level 1 (HAL) — glue between ST's USB device library and the PCD.
 *
 * Pure plumbing: every function forwards a library call to the HAL, or a HAL
 * callback back to the library. There is no application logic here.
 */
#include "usbd_core.h"
#include "usbd_msc.h"
#include "bsp.h"

PCD_HandleTypeDef hbsp_pcd;

/* ------------------------------------------------------------------------ */
/* Static allocation                                                        */
/* ------------------------------------------------------------------------ */

/** One MSC handle, aligned for the 32-bit accesses the class makes. */
static uint32_t s_class_mem[(sizeof(USBD_MSC_BOT_HandleTypeDef) / 4u) + 1u];

void *USBD_static_malloc(uint32_t size)
{
    (void)size;   /* only ever one allocation, sized at compile time */
    return s_class_mem;
}

void USBD_static_free(void *p)
{
    (void)p;
}

/* ------------------------------------------------------------------------ */
/* HAL PCD callbacks -> library                                             */
/* ------------------------------------------------------------------------ */

void HAL_PCD_SetupStageCallback(PCD_HandleTypeDef *h)
{
    (void)USBD_LL_SetupStage((USBD_HandleTypeDef *)h->pData, (uint8_t *)h->Setup);
}

void HAL_PCD_DataOutStageCallback(PCD_HandleTypeDef *h, uint8_t epnum)
{
    (void)USBD_LL_DataOutStage((USBD_HandleTypeDef *)h->pData, epnum,
                               h->OUT_ep[epnum].xfer_buff);
}

void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef *h, uint8_t epnum)
{
    (void)USBD_LL_DataInStage((USBD_HandleTypeDef *)h->pData, epnum,
                              h->IN_ep[epnum].xfer_buff);
}

void HAL_PCD_SOFCallback(PCD_HandleTypeDef *h)
{
    (void)USBD_LL_SOF((USBD_HandleTypeDef *)h->pData);
}

void HAL_PCD_ResetCallback(PCD_HandleTypeDef *h)
{
    (void)USBD_LL_SetSpeed((USBD_HandleTypeDef *)h->pData, USBD_SPEED_FULL);
    (void)USBD_LL_Reset((USBD_HandleTypeDef *)h->pData);
}

void HAL_PCD_SuspendCallback(PCD_HandleTypeDef *h)
{
    (void)USBD_LL_Suspend((USBD_HandleTypeDef *)h->pData);

    /* The host has parked the bus. Deep sleep is not an option here: the
     * device must stay enumerated and answer resume, so this is left to the
     * application's normal idle path. */
}

void HAL_PCD_ResumeCallback(PCD_HandleTypeDef *h)
{
    (void)USBD_LL_Resume((USBD_HandleTypeDef *)h->pData);
}

void HAL_PCD_ISOOUTIncompleteCallback(PCD_HandleTypeDef *h, uint8_t epnum)
{
    (void)USBD_LL_IsoOUTIncomplete((USBD_HandleTypeDef *)h->pData, epnum);
}

void HAL_PCD_ISOINIncompleteCallback(PCD_HandleTypeDef *h, uint8_t epnum)
{
    (void)USBD_LL_IsoINIncomplete((USBD_HandleTypeDef *)h->pData, epnum);
}

void HAL_PCD_ConnectCallback(PCD_HandleTypeDef *h)
{
    (void)USBD_LL_DevConnected((USBD_HandleTypeDef *)h->pData);
}

void HAL_PCD_DisconnectCallback(PCD_HandleTypeDef *h)
{
    (void)USBD_LL_DevDisconnected((USBD_HandleTypeDef *)h->pData);
}

/* ------------------------------------------------------------------------ */
/* Library -> HAL                                                           */
/* ------------------------------------------------------------------------ */

USBD_StatusTypeDef USBD_LL_Init(USBD_HandleTypeDef *pdev)
{
    hbsp_pcd.Instance = USB;
    hbsp_pcd.Init.dev_endpoints = 8u;
    hbsp_pcd.Init.speed = PCD_SPEED_FULL;
    hbsp_pcd.Init.phy_itface = PCD_PHY_EMBEDDED;
    hbsp_pcd.Init.Sof_enable = DISABLE;
    hbsp_pcd.Init.low_power_enable = DISABLE;
    hbsp_pcd.Init.lpm_enable = DISABLE;
    hbsp_pcd.Init.battery_charging_enable = DISABLE;

    hbsp_pcd.pData = pdev;
    pdev->pData = &hbsp_pcd;

    if (HAL_PCD_Init(&hbsp_pcd) != HAL_OK) {
        return USBD_FAIL;
    }

    /* Packet memory map. 1 kB total; control needs a pair of 64-byte buffers
     * and each bulk endpoint one more. */
    HAL_PCDEx_PMAConfig(&hbsp_pcd, 0x00u, PCD_SNG_BUF, 0x18u);
    HAL_PCDEx_PMAConfig(&hbsp_pcd, 0x80u, PCD_SNG_BUF, 0x58u);
    HAL_PCDEx_PMAConfig(&hbsp_pcd, MSC_EPOUT_ADDR, PCD_SNG_BUF, 0x98u);
    HAL_PCDEx_PMAConfig(&hbsp_pcd, MSC_EPIN_ADDR, PCD_SNG_BUF, 0xD8u);

    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_DeInit(USBD_HandleTypeDef *pdev)
{
    return (HAL_PCD_DeInit(pdev->pData) == HAL_OK) ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef USBD_LL_Start(USBD_HandleTypeDef *pdev)
{
    return (HAL_PCD_Start(pdev->pData) == HAL_OK) ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef USBD_LL_Stop(USBD_HandleTypeDef *pdev)
{
    return (HAL_PCD_Stop(pdev->pData) == HAL_OK) ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef USBD_LL_OpenEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr,
                                  uint8_t ep_type, uint16_t ep_mps)
{
    return (HAL_PCD_EP_Open(pdev->pData, ep_addr, ep_mps, ep_type) == HAL_OK)
           ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef USBD_LL_CloseEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    return (HAL_PCD_EP_Close(pdev->pData, ep_addr) == HAL_OK) ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef USBD_LL_FlushEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    return (HAL_PCD_EP_Flush(pdev->pData, ep_addr) == HAL_OK) ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef USBD_LL_StallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    return (HAL_PCD_EP_SetStall(pdev->pData, ep_addr) == HAL_OK) ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef USBD_LL_ClearStallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    return (HAL_PCD_EP_ClrStall(pdev->pData, ep_addr) == HAL_OK) ? USBD_OK : USBD_FAIL;
}

uint8_t USBD_LL_IsStallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    PCD_HandleTypeDef *h = (PCD_HandleTypeDef *)pdev->pData;

    if ((ep_addr & 0x80u) == 0x80u) {
        return h->IN_ep[ep_addr & 0x7Fu].is_stall;
    }
    return h->OUT_ep[ep_addr & 0x7Fu].is_stall;
}

USBD_StatusTypeDef USBD_LL_SetUSBAddress(USBD_HandleTypeDef *pdev, uint8_t dev_addr)
{
    return (HAL_PCD_SetAddress(pdev->pData, dev_addr) == HAL_OK) ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef USBD_LL_Transmit(USBD_HandleTypeDef *pdev, uint8_t ep_addr,
                                    uint8_t *pbuf, uint32_t size)
{
    return (HAL_PCD_EP_Transmit(pdev->pData, ep_addr, pbuf, size) == HAL_OK)
           ? USBD_OK : USBD_FAIL;
}

USBD_StatusTypeDef USBD_LL_PrepareReceive(USBD_HandleTypeDef *pdev, uint8_t ep_addr,
                                          uint8_t *pbuf, uint32_t size)
{
    return (HAL_PCD_EP_Receive(pdev->pData, ep_addr, pbuf, size) == HAL_OK)
           ? USBD_OK : USBD_FAIL;
}

uint32_t USBD_LL_GetRxDataSize(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    return HAL_PCD_EP_GetRxCount(pdev->pData, ep_addr);
}

void USBD_LL_Delay(uint32_t Delay)
{
    HAL_Delay(Delay);
}

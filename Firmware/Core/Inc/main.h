/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32l4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define PWR_BTN_Pin GPIO_PIN_0
#define PWR_BTN_GPIO_Port GPIOA
#define PWR_BTN_EXTI_IRQn EXTI0_IRQn
#define LED_GREEN_Pin GPIO_PIN_2
#define LED_GREEN_GPIO_Port GPIOA
#define LED_RED_Pin GPIO_PIN_3
#define LED_RED_GPIO_Port GPIOA
#define VIB_EN_Pin GPIO_PIN_4
#define VIB_EN_GPIO_Port GPIOA
#define BATT_SENSE_Pin GPIO_PIN_7
#define BATT_SENSE_GPIO_Port GPIOA
#define NFC_NSS_Pin GPIO_PIN_0
#define NFC_NSS_GPIO_Port GPIOB
#define NFC_IRQ_Pin GPIO_PIN_1
#define NFC_IRQ_GPIO_Port GPIOB
#define NFC_IRQ_EXTI_IRQn EXTI1_IRQn
#define USB_VBUS_Pin GPIO_PIN_9
#define USB_VBUS_GPIO_Port GPIOA
#define USB_VBUS_EXTI_IRQn EXTI9_5_IRQn
#define PN532_NSS_Pin GPIO_PIN_15
#define PN532_NSS_GPIO_Port GPIOA
#define NFC_SCK_Pin GPIO_PIN_3
#define NFC_SCK_GPIO_Port GPIOB
#define NFC_MISO_Pin GPIO_PIN_4
#define NFC_MISO_GPIO_Port GPIOB
#define NFC_MOSI_Pin GPIO_PIN_5
#define NFC_MOSI_GPIO_Port GPIOB
#define PN532_IRQ_Pin GPIO_PIN_6
#define PN532_IRQ_GPIO_Port GPIOB
#define PN532_IRQ_EXTI_IRQn EXTI9_5_IRQn
#define PVD_IN_Pin GPIO_PIN_7
#define PVD_IN_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */

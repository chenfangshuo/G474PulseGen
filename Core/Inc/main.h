/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
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
#include "stm32g4xx_hal.h"

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
/* 12V 功率负载开关控制宏 (PA12 - TPS22810 EN, R57下拉, 1=通, 0=断+QOD泄放) */
#define LOADSW_GPIO_Port          GPIOA
#define LOADSW_Pin                GPIO_PIN_12
#define LOADSW_ENABLE()           HAL_GPIO_WritePin(LOADSW_GPIO_Port, LOADSW_Pin, GPIO_PIN_SET)
#define LOADSW_DISABLE()          HAL_GPIO_WritePin(LOADSW_GPIO_Port, LOADSW_Pin, GPIO_PIN_RESET)

/* LTC4421 电源状态检测宏 (PC14/PC15 经 Q6/Q7 反相, 高电平=对应通道导通) */
#define PWR1_DT_GPIO_Port         GPIOC
#define PWR1_DT_Pin               GPIO_PIN_14
#define PWR2_DT_GPIO_Port         GPIOC
#define PWR2_DT_Pin               GPIO_PIN_15
#define LTC_IS_PWR1_VALID()       (HAL_GPIO_ReadPin(PWR1_DT_GPIO_Port, PWR1_DT_Pin) == GPIO_PIN_SET)
#define LTC_IS_PWR2_VALID()       (HAL_GPIO_ReadPin(PWR2_DT_GPIO_Port, PWR2_DT_Pin) == GPIO_PIN_SET)
#define LTC_IS_ANY_PWR_VALID()    (LTC_IS_PWR1_VALID() || LTC_IS_PWR2_VALID())
/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */

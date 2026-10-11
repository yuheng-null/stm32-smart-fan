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
#include "stm32f4xx_hal.h"

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

void HAL_TIM_MspPostInit(TIM_HandleTypeDef *htim);

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* CubeMX 把外设句柄定义在 main.c 里，却没有在任何头文件里声明它们。
   bsp/ 要用到这些句柄，所以在这里统一补上 extern。
   写在 USER CODE 块内：CubeMX 重新生成时会原样保留。 */
extern UART_HandleTypeDef huart1;
extern ADC_HandleTypeDef  hadc1;
extern TIM_HandleTypeDef  htim1;   /* 风扇 PWM（TIM1_CH1 → PA8） */
extern TIM_HandleTypeDef  htim2;   /* 风扇测速（TIM2_CH2 ← PA1） */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define KEY1_Pin GPIO_PIN_0
#define KEY1_GPIO_Port GPIOC
#define KEY2_Pin GPIO_PIN_1
#define KEY2_GPIO_Port GPIOC
#define FAN_TACH_Pin GPIO_PIN_1
#define FAN_TACH_GPIO_Port GPIOA
#define RELAY_IN_Pin GPIO_PIN_0
#define RELAY_IN_GPIO_Port GPIOB
#define BUZZER_IO_Pin GPIO_PIN_1
#define BUZZER_IO_GPIO_Port GPIOB
#define IR_RX_Pin GPIO_PIN_7
#define IR_RX_GPIO_Port GPIOC
#define IR_RX_EXTI_IRQn EXTI9_5_IRQn
#define FAN_PWM_Pin GPIO_PIN_8
#define FAN_PWM_GPIO_Port GPIOA
#define OLED_SCL_Pin GPIO_PIN_6
#define OLED_SCL_GPIO_Port GPIOB
#define OLED_SDA_Pin GPIO_PIN_7
#define OLED_SDA_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */

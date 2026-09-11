/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gpio.h
  * @brief   This file contains all the function prototypes for
  *          the gpio.c file
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
#ifndef __GPIO_H__
#define __GPIO_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* USER CODE BEGIN Private defines */

typedef struct
{
  uint32_t sample_count;
  uint32_t program_low_samples;
  uint32_t init_low_samples;
  uint32_t done_low_samples;
  uint32_t init_fall_count;
  uint32_t done_rise_count;
  uint32_t done_fall_count;
  uint8_t program_b;
  uint8_t init_b;
  uint8_t done;
  uint8_t pe3_mode;
  uint8_t pe3_open_drain;
  uint8_t pe3_odr;
  uint8_t pe4_mode;
  uint8_t pe4_odr;
  uint8_t initialized;
  uint8_t previous_init_b;
  uint8_t previous_done;
} FPGA_ConfigDebug_t;

extern volatile FPGA_ConfigDebug_t fpga_cfg_dbg;

/* USER CODE END Private defines */

void MX_GPIO_Init(void);

/* USER CODE BEGIN Prototypes */

void FPGA_ConfigPins_Sample(void);

/* USER CODE END Prototypes */

#ifdef __cplusplus
}
#endif
#endif /*__ GPIO_H__ */


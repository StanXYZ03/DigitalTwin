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
#include "stm32h7xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */
typedef struct
{
  uint32_t magic;
  uint32_t reason;
  uint32_t count;
  uint32_t ipsr;
  uint32_t cfsr;
  uint32_t hfsr;
  uint32_t dfsr;
  uint32_t afsr;
  uint32_t mmfar;
  uint32_t bfar;
  uint32_t active_irq;
  uint32_t assert_line;
  const char *assert_file;
} FaultDebug_t;

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */
extern volatile FaultDebug_t fault_dbg;

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/

/* USER CODE BEGIN Private defines */

/* Legacy diagnostic selector retained for source compatibility.  The normal
 * build now always uses the deterministic PI-before-configuration sequence. */
#define FPGA_CONFIG_RELEASE_DIAG  0U

/* The bridge already straps Artix-7 M[2:0]=001 with 1 kOhm resistors.  Keep
 * PROGRAM_B released and let the FPGA cold-boot directly from SPI Flash;
 * pulsing PROGRAM_B again after startup is unnecessary and can destroy an
 * otherwise successful boot.  PI key injection is initialized independently
 * by M0_DataSource_Init(). */
#define FPGA_EXTERNAL_JTAG_PASSIVE  0U

/* 1 = keep the Lattice/FMC data bridge disconnected after FPGA startup and
 *     skip all status transactions.  This isolates LCD scanout from FMC bus
 *     activity while retaining the proven Artix-7 configuration sequence. */
#define FMC_BRIDGE_ISOLATION_DIAG  0U

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */

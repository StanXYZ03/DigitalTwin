#ifndef FPGA_AUTOCONFIG_H
#define FPGA_AUTOCONFIG_H

#include "stm32h7xx_hal.h"
#include <stdint.h>

typedef enum
{
  FPGA_AUTOCONFIG_IDLE = 0,
  FPGA_AUTOCONFIG_SET_MODE,
  FPGA_AUTOCONFIG_RESET,
  FPGA_AUTOCONFIG_WAIT_INIT,
  FPGA_AUTOCONFIG_WAIT_DONE,
  FPGA_AUTOCONFIG_SUCCESS,
  FPGA_AUTOCONFIG_I2C_ERROR,
  FPGA_AUTOCONFIG_INIT_TIMEOUT,
  FPGA_AUTOCONFIG_DONE_TIMEOUT
} FPGA_AutoConfigState;

typedef struct
{
  volatile uint32_t run_count;
  volatile uint32_t state;
  volatile int32_t i2c_result;
  volatile uint32_t pcal_out1;
  volatile uint32_t pcal_cfg1;
  volatile uint32_t mux_enabled;
  volatile uint32_t config_attempts;
  volatile uint32_t init_low_seen;
  volatile uint32_t program_b;
  volatile uint32_t init_b;
  volatile uint32_t done;
  volatile uint32_t pi_init_count;
  volatile int32_t pi_i2c_result;
  volatile uint32_t pi_ready;
  volatile uint32_t pi_first_done;
  volatile uint32_t pi_last_value;
} FPGA_AutoConfigDebug;

extern volatile FPGA_AutoConfigDebug fpga_autoconfig_dbg;

/* Select M[2:0]=001 through the bridge mux and reboot Artix-7 from SPI Flash. */
HAL_StatusTypeDef FPGA_AutoConfig_Run(void);

/* Select and continuously hold M[2:0]=001 without ever driving PROGRAM_B. */
HAL_StatusTypeDef FPGA_ConfigModeHold_Run(void);

/* Return every STM32 branch shared with the external Xilinx JTAG/configuration
 * connector to a passive state.  A future MCU-JTAG service may claim these
 * pins temporarily and must call this again when it exits. */
void FPGA_ExternalJtagRelease(void);

/* The PI expander shares PB7/PB8 with the configuration-mode expander.
 * FPGA_AutoConfig_Run establishes PI=0 before it asserts PROGRAM_B. */
HAL_StatusTypeDef FPGA_PI_Init(void);
HAL_StatusTypeDef FPGA_PI_Write(uint16_t value);

#endif

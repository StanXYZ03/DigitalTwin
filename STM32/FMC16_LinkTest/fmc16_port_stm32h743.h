#ifndef FMC16_PORT_STM32H743_H
#define FMC16_PORT_STM32H743_H

#include <stdint.h>

typedef enum {
    FMC16_STM32_OK = 0,
    FMC16_STM32_ERR_CLOCK = -100,
    FMC16_STM32_ERR_HAL = -101
} fmc16_stm32_result_t;

void fmc16_stm32h743_mpu_config(void);
fmc16_stm32_result_t fmc16_stm32h743_init(void);
uint16_t fmc16_stm32h743_read_word(void *context);
void fmc16_stm32h743_write_word(void *context, uint16_t word);

#endif

#ifndef LCD_STATUS_H
#define LCD_STATUS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "m0_data_source.h"
#include "stm32h7xx_hal.h"
#include <stdint.h>

typedef enum
{
    LCD_STATUS_WAIT_LINK = 0,
    LCD_STATUS_LINK_UP,
    LCD_STATUS_SEND_OK,
    LCD_STATUS_SEND_ERROR
} LCD_StatusNetworkState;

HAL_StatusTypeDef LCD_Status_Init(void);
void LCD_Status_Update(const M0_DataSnapshot *snapshot,
                       LCD_StatusNetworkState network_state,
                       uint32_t send_count,
                       uint32_t error_count);

#ifdef __cplusplus
}
#endif

#endif /* LCD_STATUS_H */

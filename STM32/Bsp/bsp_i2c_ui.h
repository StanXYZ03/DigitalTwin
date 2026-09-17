#ifndef BSP_I2C_UI_H
#define BSP_I2C_UI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Shared software-I2C bus on PB7 (SDA) and PB8 (SCL).  Every public
 * transaction is serialized so the FPGA expanders and board-monitoring
 * devices cannot interleave bus cycles. */
void BSP_I2C_UI_Init(void);
void BSP_I2C_UI_BusRecover(void);
int32_t BSP_I2C_UI_Probe(uint8_t address7);
int32_t BSP_I2C_UI_WriteReg(uint8_t address7, uint8_t reg, uint8_t value);
int32_t BSP_I2C_UI_ReadReg(uint8_t address7, uint8_t reg, uint8_t *value);
int32_t BSP_I2C_UI_ReadBytes(uint8_t address7, uint8_t reg,
                            uint8_t *data, uint32_t length);
int32_t BSP_I2C_UI_WriteBytes(uint8_t address7, uint8_t reg,
                             const uint8_t *data, uint32_t length);
int32_t BSP_I2C_UI_WriteRaw(uint8_t address7,
                           const uint8_t *data, uint32_t length);
int32_t BSP_I2C_UI_ReadRaw(uint8_t address7,
                          uint8_t *data, uint32_t length);

#ifdef __cplusplus
}
#endif

#endif /* BSP_I2C_UI_H */

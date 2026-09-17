#include "bsp_i2c_ui.h"

#include "main.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#define I2C_UI_PORT       GPIOB
#define I2C_UI_SDA_PIN    GPIO_PIN_7
#define I2C_UI_SCL_PIN    GPIO_PIN_8
#define I2C_UI_DELAY_US   2U

/* The bridge-board SHT40 and U10 are behind PCA9617 U2.  Its EN net is
 * FMC_NWAIT_LCD_B2 (PD6).  This target neither uses FMC NWAIT nor LTDC B2,
 * so PD6 is dedicated to keeping the I2C buffer enabled here. */
#define I2C_UI_BUFFER_EN_PORT GPIOD
#define I2C_UI_BUFFER_EN_PIN  GPIO_PIN_6

static SemaphoreHandle_t i2c_ui_mutex;
static uint8_t i2c_ui_initialized;

static void I2C_UI_DelayUs(uint32_t microseconds)
{
    uint32_t start;
    uint32_t ticks;

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    start = DWT->CYCCNT;
    ticks = (SystemCoreClock / 1000000U) * microseconds;
    while ((uint32_t)(DWT->CYCCNT - start) < ticks)
    {
    }
}

static void I2C_UI_Delay(void)
{
    I2C_UI_DelayUs(I2C_UI_DELAY_US);
}

static void I2C_UI_Lock(void)
{
    if ((i2c_ui_mutex != NULL) &&
        (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING))
    {
        (void)xSemaphoreTake(i2c_ui_mutex, portMAX_DELAY);
    }
}

static void I2C_UI_Unlock(void)
{
    if ((i2c_ui_mutex != NULL) &&
        (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING))
    {
        (void)xSemaphoreGive(i2c_ui_mutex);
    }
}

static void I2C_UI_Start(void)
{
    HAL_GPIO_WritePin(I2C_UI_PORT, I2C_UI_SDA_PIN | I2C_UI_SCL_PIN,
                      GPIO_PIN_SET);
    I2C_UI_Delay();
    HAL_GPIO_WritePin(I2C_UI_PORT, I2C_UI_SDA_PIN, GPIO_PIN_RESET);
    I2C_UI_Delay();
    HAL_GPIO_WritePin(I2C_UI_PORT, I2C_UI_SCL_PIN, GPIO_PIN_RESET);
}

static void I2C_UI_Stop(void)
{
    HAL_GPIO_WritePin(I2C_UI_PORT, I2C_UI_SDA_PIN, GPIO_PIN_RESET);
    I2C_UI_Delay();
    HAL_GPIO_WritePin(I2C_UI_PORT, I2C_UI_SCL_PIN, GPIO_PIN_SET);
    I2C_UI_Delay();
    HAL_GPIO_WritePin(I2C_UI_PORT, I2C_UI_SDA_PIN, GPIO_PIN_SET);
    I2C_UI_Delay();
}

static uint8_t I2C_UI_SendByte(uint8_t value)
{
    uint32_t bit;
    uint8_t nack;

    for (bit = 0U; bit < 8U; ++bit)
    {
        HAL_GPIO_WritePin(I2C_UI_PORT, I2C_UI_SDA_PIN,
                          ((value & 0x80U) != 0U) ? GPIO_PIN_SET
                                                 : GPIO_PIN_RESET);
        I2C_UI_Delay();
        HAL_GPIO_WritePin(I2C_UI_PORT, I2C_UI_SCL_PIN, GPIO_PIN_SET);
        I2C_UI_Delay();
        HAL_GPIO_WritePin(I2C_UI_PORT, I2C_UI_SCL_PIN, GPIO_PIN_RESET);
        value <<= 1U;
    }

    HAL_GPIO_WritePin(I2C_UI_PORT, I2C_UI_SDA_PIN, GPIO_PIN_SET);
    I2C_UI_Delay();
    HAL_GPIO_WritePin(I2C_UI_PORT, I2C_UI_SCL_PIN, GPIO_PIN_SET);
    I2C_UI_Delay();
    nack = (HAL_GPIO_ReadPin(I2C_UI_PORT, I2C_UI_SDA_PIN) == GPIO_PIN_SET)
               ? 1U : 0U;
    HAL_GPIO_WritePin(I2C_UI_PORT, I2C_UI_SCL_PIN, GPIO_PIN_RESET);
    I2C_UI_Delay();
    return nack;
}

static uint8_t I2C_UI_ReadByte(uint8_t send_ack)
{
    uint32_t bit;
    uint8_t value = 0U;

    HAL_GPIO_WritePin(I2C_UI_PORT, I2C_UI_SDA_PIN, GPIO_PIN_SET);
    for (bit = 0U; bit < 8U; ++bit)
    {
        value <<= 1U;
        HAL_GPIO_WritePin(I2C_UI_PORT, I2C_UI_SCL_PIN, GPIO_PIN_SET);
        I2C_UI_Delay();
        if (HAL_GPIO_ReadPin(I2C_UI_PORT, I2C_UI_SDA_PIN) == GPIO_PIN_SET)
        {
            value |= 1U;
        }
        HAL_GPIO_WritePin(I2C_UI_PORT, I2C_UI_SCL_PIN, GPIO_PIN_RESET);
        I2C_UI_Delay();
    }

    HAL_GPIO_WritePin(I2C_UI_PORT, I2C_UI_SDA_PIN,
                      (send_ack != 0U) ? GPIO_PIN_RESET : GPIO_PIN_SET);
    I2C_UI_Delay();
    HAL_GPIO_WritePin(I2C_UI_PORT, I2C_UI_SCL_PIN, GPIO_PIN_SET);
    I2C_UI_Delay();
    HAL_GPIO_WritePin(I2C_UI_PORT, I2C_UI_SCL_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(I2C_UI_PORT, I2C_UI_SDA_PIN, GPIO_PIN_SET);
    I2C_UI_Delay();
    return value;
}

static void I2C_UI_RecoverUnlocked(void)
{
    uint32_t pulse;

    HAL_GPIO_WritePin(I2C_UI_PORT, I2C_UI_SDA_PIN, GPIO_PIN_SET);
    for (pulse = 0U; pulse < 9U; ++pulse)
    {
        HAL_GPIO_WritePin(I2C_UI_PORT, I2C_UI_SCL_PIN, GPIO_PIN_SET);
        I2C_UI_Delay();
        HAL_GPIO_WritePin(I2C_UI_PORT, I2C_UI_SCL_PIN, GPIO_PIN_RESET);
        I2C_UI_Delay();
    }
    I2C_UI_Stop();
}

void BSP_I2C_UI_Init(void)
{
    GPIO_InitTypeDef gpio;

    if (i2c_ui_initialized != 0U)
    {
        return;
    }
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    HAL_GPIO_WritePin(I2C_UI_BUFFER_EN_PORT, I2C_UI_BUFFER_EN_PIN,
                      GPIO_PIN_RESET);
    gpio.Pin = I2C_UI_BUFFER_EN_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    gpio.Alternate = 0U;
    HAL_GPIO_Init(I2C_UI_BUFFER_EN_PORT, &gpio);

    HAL_GPIO_WritePin(I2C_UI_PORT, I2C_UI_SDA_PIN | I2C_UI_SCL_PIN,
                      GPIO_PIN_SET);
    gpio.Pin = I2C_UI_SDA_PIN | I2C_UI_SCL_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_OD;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    gpio.Alternate = 0U;
    HAL_GPIO_Init(I2C_UI_PORT, &gpio);
    I2C_UI_DelayUs(1000U);
    HAL_GPIO_WritePin(I2C_UI_BUFFER_EN_PORT, I2C_UI_BUFFER_EN_PIN,
                      GPIO_PIN_SET);
    I2C_UI_DelayUs(1000U);
    if (i2c_ui_mutex == NULL)
    {
        i2c_ui_mutex = xSemaphoreCreateMutex();
    }
    i2c_ui_initialized = 1U;
}

int32_t BSP_I2C_UI_Probe(uint8_t address7)
{
    int32_t result;

    if (address7 > 0x7FU)
    {
        return -1;
    }
    BSP_I2C_UI_Init();
    I2C_UI_Lock();
    I2C_UI_Start();
    result = (int32_t)I2C_UI_SendByte((uint8_t)(address7 << 1U));
    I2C_UI_Stop();
    I2C_UI_Unlock();
    return (result == 0) ? 0 : -1;
}

void BSP_I2C_UI_BusRecover(void)
{
    BSP_I2C_UI_Init();
    I2C_UI_Lock();
    I2C_UI_RecoverUnlocked();
    I2C_UI_Unlock();
}

int32_t BSP_I2C_UI_WriteRaw(uint8_t address7,
                           const uint8_t *data, uint32_t length)
{
    uint32_t index;
    int32_t result = 0;

    if ((data == NULL) || (length == 0U))
    {
        return -1;
    }
    BSP_I2C_UI_Init();
    I2C_UI_Lock();
    I2C_UI_Start();
    result |= (int32_t)I2C_UI_SendByte((uint8_t)(address7 << 1U));
    for (index = 0U; index < length; ++index)
    {
        result |= (int32_t)I2C_UI_SendByte(data[index]);
    }
    I2C_UI_Stop();
    if (result != 0)
    {
        I2C_UI_RecoverUnlocked();
    }
    I2C_UI_Unlock();
    return (result == 0) ? 0 : -1;
}

int32_t BSP_I2C_UI_ReadRaw(uint8_t address7,
                          uint8_t *data, uint32_t length)
{
    uint32_t index;
    int32_t result = 0;

    if ((data == NULL) || (length == 0U))
    {
        return -1;
    }
    BSP_I2C_UI_Init();
    I2C_UI_Lock();
    I2C_UI_Start();
    result = (int32_t)I2C_UI_SendByte((uint8_t)((address7 << 1U) | 1U));
    if (result == 0)
    {
        for (index = 0U; index < length; ++index)
        {
            data[index] = I2C_UI_ReadByte(
                (index + 1U < length) ? 1U : 0U);
        }
    }
    I2C_UI_Stop();
    if (result != 0)
    {
        I2C_UI_RecoverUnlocked();
    }
    I2C_UI_Unlock();
    return (result == 0) ? 0 : -1;
}

int32_t BSP_I2C_UI_WriteBytes(uint8_t address7, uint8_t reg,
                             const uint8_t *data, uint32_t length)
{
    uint32_t index;
    int32_t result = 0;

    if ((data == NULL) || (length == 0U))
    {
        return -1;
    }
    BSP_I2C_UI_Init();
    I2C_UI_Lock();
    I2C_UI_Start();
    result |= (int32_t)I2C_UI_SendByte((uint8_t)(address7 << 1U));
    result |= (int32_t)I2C_UI_SendByte(reg);
    for (index = 0U; index < length; ++index)
    {
        result |= (int32_t)I2C_UI_SendByte(data[index]);
    }
    I2C_UI_Stop();
    if (result != 0)
    {
        I2C_UI_RecoverUnlocked();
    }
    I2C_UI_Unlock();
    return (result == 0) ? 0 : -1;
}

int32_t BSP_I2C_UI_WriteReg(uint8_t address7, uint8_t reg, uint8_t value)
{
    return BSP_I2C_UI_WriteBytes(address7, reg, &value, 1U);
}

int32_t BSP_I2C_UI_ReadReg(uint8_t address7, uint8_t reg, uint8_t *value)
{
    return BSP_I2C_UI_ReadBytes(address7, reg, value, 1U);
}

int32_t BSP_I2C_UI_ReadBytes(uint8_t address7, uint8_t reg,
                            uint8_t *data, uint32_t length)
{
    uint32_t index;
    int32_t result = 0;

    if ((data == NULL) || (length == 0U))
    {
        return -1;
    }
    BSP_I2C_UI_Init();
    I2C_UI_Lock();
    I2C_UI_Start();
    result |= (int32_t)I2C_UI_SendByte((uint8_t)(address7 << 1U));
    result |= (int32_t)I2C_UI_SendByte(reg);
    if (result == 0)
    {
        I2C_UI_Start();
        result |= (int32_t)I2C_UI_SendByte(
            (uint8_t)((address7 << 1U) | 1U));
    }
    if (result == 0)
    {
        for (index = 0U; index < length; ++index)
        {
            data[index] = I2C_UI_ReadByte(
                (index + 1U < length) ? 1U : 0U);
        }
    }
    I2C_UI_Stop();
    if (result != 0)
    {
        I2C_UI_RecoverUnlocked();
    }
    I2C_UI_Unlock();
    return (result == 0) ? 0 : -1;
}

#include "fmc16_port_stm32h743.h"

#include "stm32h7xx_hal.h"
#include "fmc16_link_test_debug.h"

#define FMC16_BASE_ADDRESS 0x60000000UL

static SRAM_HandleTypeDef fmc16_hsram;

static void fmc16_gpio_init(void)
{
    GPIO_InitTypeDef gpio = {0};

    __HAL_RCC_FMC_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();

    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF12_FMC;

    gpio.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_8 | GPIO_PIN_9 |
               GPIO_PIN_10 | GPIO_PIN_14 | GPIO_PIN_15;
    HAL_GPIO_Init(GPIOD, &gpio);

    gpio.Pin = GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 |
               GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14 |
               GPIO_PIN_15;
    HAL_GPIO_Init(GPIOE, &gpio);

    gpio.Pin = GPIO_PIN_4 | GPIO_PIN_5;
    HAL_GPIO_Init(GPIOD, &gpio);

    gpio.Alternate = GPIO_AF9_FMC;
    gpio.Pin = GPIO_PIN_7;
    HAL_GPIO_Init(GPIOC, &gpio);
}

void fmc16_stm32h743_mpu_config(void)
{
    MPU_Region_InitTypeDef region = {0};

    HAL_MPU_Disable();
    region.Enable = MPU_REGION_ENABLE;
    /* Region 1 belongs to the Ethernet D2 SRAM mapping in this application. */
    region.Number = MPU_REGION_NUMBER2;
    region.BaseAddress = FMC16_BASE_ADDRESS;
    region.Size = ARM_MPU_REGION_SIZE_64KB;
    region.SubRegionDisable = 0x00U;
    region.TypeExtField = MPU_TEX_LEVEL0;
    region.AccessPermission = MPU_REGION_FULL_ACCESS;
    region.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
    region.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
    region.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
    region.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;
    HAL_MPU_ConfigRegion(&region);
    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}

fmc16_stm32_result_t fmc16_stm32h743_init(void)
{
    FMC_NORSRAM_TimingTypeDef read_timing = {0};
    FMC_NORSRAM_TimingTypeDef write_timing = {0};
    /* This CubeMX application clocks FMC directly from PLL2 at 150 MHz;
     * D1CPRE therefore does not define the FMC kernel frequency. */
    fmc16_link_test_dbg.sysclk_hz = HAL_RCC_GetSysClockFreq();
    fmc16_link_test_dbg.fmcclk_hz =
        HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_FMC);
    fmc16_link_test_dbg.rcc_d1cfgr = RCC->D1CFGR;
    fmc16_link_test_dbg.rcc_pll2divr = RCC->PLL2DIVR;
    /* This HAL revision returns 0 for RCC_PERIPHCLK_FMC even though FMC is
     * selected from the running PLL2. Keep the raw clock registers above for
     * diagnosis and reject only an incompatible system clock here. */
    if (fmc16_link_test_dbg.sysclk_hz != 400000000UL) {
        return FMC16_STM32_ERR_CLOCK;
    }

    fmc16_gpio_init();
    fmc16_hsram.Instance = FMC_NORSRAM_DEVICE;
    fmc16_hsram.Extended = FMC_NORSRAM_EXTENDED_DEVICE;
    fmc16_hsram.Init.NSBank = FMC_NORSRAM_BANK1;
    fmc16_hsram.Init.DataAddressMux = FMC_DATA_ADDRESS_MUX_DISABLE;
    fmc16_hsram.Init.MemoryType = FMC_MEMORY_TYPE_SRAM;
    fmc16_hsram.Init.MemoryDataWidth = FMC_NORSRAM_MEM_BUS_WIDTH_16;
    fmc16_hsram.Init.BurstAccessMode = FMC_BURST_ACCESS_MODE_DISABLE;
    fmc16_hsram.Init.WaitSignalPolarity = FMC_WAIT_SIGNAL_POLARITY_LOW;
    fmc16_hsram.Init.WaitSignalActive = FMC_WAIT_TIMING_BEFORE_WS;
    fmc16_hsram.Init.WriteOperation = FMC_WRITE_OPERATION_ENABLE;
    fmc16_hsram.Init.WaitSignal = FMC_WAIT_SIGNAL_DISABLE;
    fmc16_hsram.Init.ExtendedMode = FMC_EXTENDED_MODE_ENABLE;
    fmc16_hsram.Init.AsynchronousWait = FMC_ASYNCHRONOUS_WAIT_DISABLE;
    fmc16_hsram.Init.WriteBurst = FMC_WRITE_BURST_DISABLE;
    fmc16_hsram.Init.ContinuousClock = FMC_CONTINUOUS_CLOCK_SYNC_ONLY;
    fmc16_hsram.Init.WriteFifo = FMC_WRITE_FIFO_DISABLE;
    fmc16_hsram.Init.PageSize = FMC_PAGE_SIZE_NONE;

    read_timing.AddressSetupTime = 4U;
    read_timing.AddressHoldTime = 6U;
    read_timing.DataSetupTime = 255U;
    read_timing.BusTurnAroundDuration = 10U;
    read_timing.CLKDivision = 2U;
    read_timing.DataLatency = 2U;
    read_timing.AccessMode = FMC_ACCESS_MODE_A;
    write_timing = read_timing;

    if (HAL_SRAM_Init(&fmc16_hsram, &read_timing, &write_timing) != HAL_OK) {
        return FMC16_STM32_ERR_HAL;
    }
    return FMC16_STM32_OK;
}

uint16_t fmc16_stm32h743_read_word(void *context)
{
    uint16_t word;

    (void)context;
    word = *(volatile uint16_t *)FMC16_BASE_ADDRESS;
    fmc16_link_test_dbg.last_raw_word = word;
    fmc16_link_test_dbg.rx_word_count++;
    return word;
}

void fmc16_stm32h743_write_word(void *context, uint16_t word)
{
    (void)context;
    *(volatile uint16_t *)FMC16_BASE_ADDRESS = word;
    fmc16_link_test_dbg.tx_word_count++;
}

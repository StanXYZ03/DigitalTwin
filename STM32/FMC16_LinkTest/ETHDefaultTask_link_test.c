#include "ETHDefaultTask.h"
#include "cmsis_os.h"
#include "fmc16_link_test_debug.h"
#include "fmc16_smoke.h"
#include "stm32h7xx_hal.h"

#include <stddef.h>

#define FMC16_LINK_TEST_MAGIC 0x464D4331UL

volatile FMC16_LinkTestDebug fmc16_link_test_dbg;

static void debug_clear(void)
{
    volatile uint8_t *bytes;
    size_t i;

    bytes = (volatile uint8_t *)&fmc16_link_test_dbg;
    for (i = 0U; i < sizeof(fmc16_link_test_dbg); ++i)
    {
        bytes[i] = 0U;
    }
}

void ETHDefaultTask(void const *argument)
{
    uint32_t capabilities;

    (void)argument;
    debug_clear();
    fmc16_link_test_dbg.magic = FMC16_LINK_TEST_MAGIC;
    fmc16_link_test_dbg.stage = 1U;

    /* Let the XO2 finish configuration and enqueue its three boot packets. */
    osDelay(100U);

    fmc16_link_test_dbg.bridge_odr_before = GPIOH->ODR;
    HAL_GPIO_WritePin(GPIOH, GPIO_PIN_8, GPIO_PIN_RESET);
    __DSB();
    osDelay(1U);
    fmc16_link_test_dbg.bridge_odr_enabled = GPIOH->ODR;

    capabilities = 0U;
    fmc16_link_test_dbg.run_count = 1U;
    fmc16_link_test_dbg.smoke_result =
        fmc16_minimal_smoke_run_with_capabilities(&capabilities);
    fmc16_link_test_dbg.peer_capabilities = capabilities;
    fmc16_link_test_dbg.fmc_bcr1 = FMC_Bank1_R->BTCR[0];
    fmc16_link_test_dbg.fmc_btr1 = FMC_Bank1_R->BTCR[1];
    fmc16_link_test_dbg.fmc_bwtr1 = FMC_Bank1E_R->BWTR[0];

    __DSB();
    HAL_GPIO_WritePin(GPIOH, GPIO_PIN_8, GPIO_PIN_SET);
    __DSB();
    fmc16_link_test_dbg.bridge_odr_after = GPIOH->ODR;
    if (fmc16_link_test_dbg.smoke_result == 0)
    {
        fmc16_link_test_dbg.stage = 100U;
    }

    /* Run exactly once so the boot-packet result remains inspectable. */
    for (;;)
    {
        osDelay(1000U);
    }
}

/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    freertos.c
  * @brief   FreeRTOS objects for the M0 Ethernet demo
  ******************************************************************************
  */
/* USER CODE END Header */

#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"
#include "board_monitor.h"
#include "board_panel.h"
#include "fpga_autoconfig.h"

osThreadId ETHHandle;
osThreadId BoardMonitorHandle;
osThreadId BoardPanelHandle;

void ETHDefaultTask(void const *argument);
void MX_FREERTOS_Init(void);

static StaticTask_t xIdleTaskTCBBuffer;
static StackType_t xIdleStack[configMINIMAL_STACK_SIZE];

void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                   StackType_t **ppxIdleTaskStackBuffer,
                                   uint32_t *pulIdleTaskStackSize)
{
    *ppxIdleTaskTCBBuffer = &xIdleTaskTCBBuffer;
    *ppxIdleTaskStackBuffer = &xIdleStack[0];
    *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

void MX_FREERTOS_Init(void)
{
    if (FPGA_JtagMaintenanceIsActive() == 0U)
    {
        BoardMonitor_Init();
        BoardPanel_Init();
    }

    osThreadDef(ETH, ETHDefaultTask, osPriorityBelowNormal, 0, 2048);
    ETHHandle = osThreadCreate(osThread(ETH), NULL);

    if (ETHHandle == NULL)
    {
        fault_dbg.reason = 22U;
        fault_dbg.count++;
        Error_Handler();
    }

    if (FPGA_JtagMaintenanceIsActive() == 0U)
    {
        osThreadDef(BoardMonitor, BoardMonitorTask, osPriorityLow, 0, 768);
        BoardMonitorHandle = osThreadCreate(osThread(BoardMonitor), NULL);
        if (BoardMonitorHandle == NULL)
        {
            fault_dbg.reason = 23U;
            fault_dbg.count++;
            Error_Handler();
        }

        osThreadDef(BoardPanel, BoardPanelTask, osPriorityLow, 0, 768);
        BoardPanelHandle = osThreadCreate(osThread(BoardPanel), NULL);
        if (BoardPanelHandle == NULL)
        {
            fault_dbg.reason = 24U;
            fault_dbg.count++;
            Error_Handler();
        }
    }
}

__weak void ETHDefaultTask(void const *argument)
{
    (void)argument;
    for (;;)
    {
        osDelay(1000U);
    }
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    fault_dbg.reason = 20U;
    fault_dbg.count++;
    fault_dbg.active_irq = (uint32_t)__get_IPSR();
    fault_dbg.assert_file = pcTaskName;
    fault_dbg.assert_line = (uint32_t)xTask;
    taskDISABLE_INTERRUPTS();
    for (;;)
    {
    }
}

void vApplicationMallocFailedHook(void)
{
    fault_dbg.reason = 21U;
    fault_dbg.count++;
    fault_dbg.active_irq = (uint32_t)__get_IPSR();
    taskDISABLE_INTERRUPTS();
    for (;;)
    {
    }
}

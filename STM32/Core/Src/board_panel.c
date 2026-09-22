#include "board_panel.h"

#include "board_monitor.h"
#include "bsp_i2c_ui.h"
#include "m0_data_source.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stm32h7xx_hal.h"
#include <string.h>

#define PANEL_STARTUP_DELAY_MS       10000U
#define PANEL_UPDATE_MS              100U
#define PANEL_USB_ACTIVE_MS          5000U

#define PCAL6524_ADDR7               0x22U
#define PCAL_REG_IN0                 0x00U
#define PCAL_REG_IN1                 0x01U
#define PCAL_REG_IN2                 0x02U
#define PCAL_REG_OUT0                0x04U
#define PCAL_REG_OUT1                0x05U
#define PCAL_REG_OUT2                0x06U
#define PCAL_REG_CFG0                0x0CU
#define PCAL_REG_CFG1                0x0DU
#define PCAL_REG_CFG2                0x0EU
#define PCAL_REG_DRIVE0              0x40U
#define PCAL_REG_DRIVE1              0x41U
#define PCAL_REG_DRIVE2              0x42U
#define PCAL_REG_DRIVE3              0x43U
#define PCAL_REG_DRIVE4              0x44U
#define PCAL_REG_DRIVE5              0x45U
#define PCAL_REG_ODC                 0x5CU

#define INA226_U10_ADDR7             0x41U
#define INA226_REG_MASK_ENABLE       0x06U
#define INA226_REG_ALERT_LIMIT       0x07U
#define INA226_MASK_SOL_LATCH        0x8001U
#define BUZZER_TRIGGER_LSB           200U
#define BUZZER_SUPPRESS_LSB          0x7FFFU

#define PANEL_MUX_ENABLE_PORT        GPIOH
#define PANEL_MUX_ENABLE_PIN         GPIO_PIN_7

/* The 58 dot-matrix route is BSW-A channel B3.  Bitmap bit N-1 represents
 * the corresponding physical DIP position (ON is driven low by PCAL6524).
 * The schematic truth table for A-B3 is:
 *   B1,B2,B5,B6,B8,B9 = ON; B3,B4,B7,B10 = OFF.
 * Thus the physical-switch bitmap is 0x01B3.  M0 uses no BSW route. */
#define PANEL_MODE_COUNTER           0U
#define PANEL_MODE_DOT_MATRIX        11U
#define PANEL_ROUTE_ALL_OFF          0x0000U
#define PANEL_ROUTE_DOT_MATRIX       0x01B3U
/* Temporary hardware-isolation switch: force the already proven M11 BSW
 * route after PCAL6524 initialization, without waiting for FMC/F10 mode
 * reconciliation.  Keep this enabled only while diagnosing the matrix. */
#define PANEL_FORCE_DOT_MATRIX_DIAG  1U

volatile BoardPanelDebug board_panel_dbg;
static BoardPanelSnapshot board_panel_snapshot;
static uint32_t board_panel_last_usb_ms;

static int32_t BoardPanel_WriteReg16(uint8_t address7, uint8_t reg,
                                     uint16_t value)
{
    uint8_t data[2];

    data[0] = (uint8_t)(value >> 8U);
    data[1] = (uint8_t)value;
    return BSP_I2C_UI_WriteBytes(address7, reg, data, 2U);
}

static uint16_t BoardPanel_NormalizeSwitches(uint16_t switches)
{
    switches &= 0x3FFFU;
    switches &= (uint16_t)~(1U << 9U); /* B10 is not routed. */
    return switches;
}

static void BoardPanel_MapSwitches(uint16_t switches,
                                   uint8_t *out0, uint8_t *out1)
{
    uint8_t p1 = 0xFFU;

    switches = BoardPanel_NormalizeSwitches(switches);
    *out0 = (uint8_t)~switches; /* B1..B8, ON is low. */
    if ((switches & (1U << 8U)) != 0U)  { p1 &= (uint8_t)~0x01U; }
    if ((switches & (1U << 10U)) != 0U) { p1 &= (uint8_t)~0x02U; }
    if ((switches & (1U << 11U)) != 0U) { p1 &= (uint8_t)~0x04U; }
    if ((switches & (1U << 13U)) != 0U) { p1 &= (uint8_t)~0x08U; }
    if ((switches & (1U << 12U)) != 0U) { p1 &= (uint8_t)~0x10U; }
    /* P1.5 remains high for Xilinx Master-SPI mode; P1.6/7 stay high. */
    *out1 = p1;
}

static void BoardPanel_SetMuxEnabled(uint8_t enabled)
{
    HAL_GPIO_WritePin(PANEL_MUX_ENABLE_PORT, PANEL_MUX_ENABLE_PIN,
                      (enabled != 0U) ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static int32_t BoardPanel_WriteSwitches(uint16_t switches)
{
    uint8_t out0;
    uint8_t out1;
    int32_t result;

    BoardPanel_MapSwitches(switches, &out0, &out1);
    result = BSP_I2C_UI_WriteReg(PCAL6524_ADDR7, PCAL_REG_OUT0, out0);
    if (result == 0)
    {
        result = BSP_I2C_UI_WriteReg(PCAL6524_ADDR7, PCAL_REG_OUT1, out1);
    }
    return result;
}

static int32_t BoardPanel_WriteYds(uint8_t on_mask)
{
    uint8_t output = (uint8_t)(~on_mask | 0xE0U);
    return BSP_I2C_UI_WriteReg(PCAL6524_ADDR7, PCAL_REG_OUT2, output);
}

static void BoardPanel_ReadPcalDiagnostics(void)
{
    static uint32_t last_read_ms;
    uint32_t now_ms = HAL_GetTick();
    uint8_t in0 = 0U;
    uint8_t in1 = 0U;
    uint8_t in2 = 0U;
    uint8_t out0 = 0U;
    uint8_t out1 = 0U;
    uint8_t out2 = 0U;
    int32_t result;

    if ((now_ms - last_read_ms) < 500U)
        return;
    last_read_ms = now_ms;

    result = BSP_I2C_UI_ReadReg(PCAL6524_ADDR7, PCAL_REG_IN0, &in0);
    if (result == 0) result = BSP_I2C_UI_ReadReg(PCAL6524_ADDR7, PCAL_REG_IN1, &in1);
    if (result == 0) result = BSP_I2C_UI_ReadReg(PCAL6524_ADDR7, PCAL_REG_IN2, &in2);
    if (result == 0) result = BSP_I2C_UI_ReadReg(PCAL6524_ADDR7, PCAL_REG_OUT0, &out0);
    if (result == 0) result = BSP_I2C_UI_ReadReg(PCAL6524_ADDR7, PCAL_REG_OUT1, &out1);
    if (result == 0) result = BSP_I2C_UI_ReadReg(PCAL6524_ADDR7, PCAL_REG_OUT2, &out2);

    taskENTER_CRITICAL();
    board_panel_dbg.pcal_read_result = result;
    board_panel_dbg.mux_enable_ph7 =
        (HAL_GPIO_ReadPin(PANEL_MUX_ENABLE_PORT, PANEL_MUX_ENABLE_PIN) ==
         GPIO_PIN_SET) ? 1U : 0U;
    if (result == 0)
    {
        board_panel_dbg.pcal_in0 = in0;
        board_panel_dbg.pcal_in1 = in1;
        board_panel_dbg.pcal_in2 = in2;
        board_panel_dbg.pcal_out0 = out0;
        board_panel_dbg.pcal_out1 = out1;
        board_panel_dbg.pcal_out2 = out2;
        board_panel_dbg.pcal_read_count++;
    }
    else
    {
        board_panel_dbg.pcal_read_error_count++;
    }
    taskEXIT_CRITICAL();
}

static int32_t BoardPanel_InitializeHardware(void)
{
    GPIO_InitTypeDef gpio;
    uint8_t alert_status[2];
    uint8_t out0;
    uint8_t out1;
    int32_t result;

    BoardPanel_MapSwitches(board_panel_snapshot.switch_bitmap, &out0, &out1);
    result = BSP_I2C_UI_WriteReg(PCAL6524_ADDR7, PCAL_REG_OUT0, out0);
    if (result == 0) result = BSP_I2C_UI_WriteReg(PCAL6524_ADDR7, PCAL_REG_OUT1, out1);
    if (result == 0) result = BSP_I2C_UI_WriteReg(PCAL6524_ADDR7, PCAL_REG_OUT2, 0xFFU);
    if (result == 0) result = BSP_I2C_UI_WriteReg(PCAL6524_ADDR7, PCAL_REG_ODC, 0x00U);
    if (result == 0) result = BSP_I2C_UI_WriteReg(PCAL6524_ADDR7, PCAL_REG_DRIVE0, 0xFFU);
    if (result == 0) result = BSP_I2C_UI_WriteReg(PCAL6524_ADDR7, PCAL_REG_DRIVE1, 0xFFU);
    if (result == 0) result = BSP_I2C_UI_WriteReg(PCAL6524_ADDR7, PCAL_REG_DRIVE2, 0xFFU);
    if (result == 0) result = BSP_I2C_UI_WriteReg(PCAL6524_ADDR7, PCAL_REG_DRIVE3, 0xFFU);
    if (result == 0) result = BSP_I2C_UI_WriteReg(PCAL6524_ADDR7, PCAL_REG_DRIVE4, 0xFFU);
    if (result == 0) result = BSP_I2C_UI_WriteReg(PCAL6524_ADDR7, PCAL_REG_DRIVE5, 0xFFU);
    if (result == 0) result = BSP_I2C_UI_WriteReg(PCAL6524_ADDR7, PCAL_REG_CFG0, 0x00U);
    if (result == 0) result = BSP_I2C_UI_WriteReg(PCAL6524_ADDR7, PCAL_REG_CFG1, 0xC0U);
    if (result == 0) result = BSP_I2C_UI_WriteReg(PCAL6524_ADDR7, PCAL_REG_CFG2, 0xE0U);
    if (result == 0)
    {
        result = BoardPanel_WriteReg16(INA226_U10_ADDR7,
                                       INA226_REG_ALERT_LIMIT,
                                       BUZZER_SUPPRESS_LSB);
    }
    if (result == 0)
    {
        result = BoardPanel_WriteReg16(INA226_U10_ADDR7,
                                       INA226_REG_MASK_ENABLE,
                                       INA226_MASK_SOL_LATCH);
    }
    if (result == 0)
    {
        /* Reading MASK/ENABLE clears a previously latched ALERT.  Program
         * the suppress limit first so reset cannot briefly retrigger it. */
        result = BSP_I2C_UI_ReadBytes(INA226_U10_ADDR7,
                                     INA226_REG_MASK_ENABLE,
                                     alert_status, 2U);
    }
    if (result != 0)
    {
        return result;
    }

    __HAL_RCC_GPIOH_CLK_ENABLE();
    gpio.Pin = PANEL_MUX_ENABLE_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    gpio.Alternate = 0U;
    HAL_GPIO_Init(PANEL_MUX_ENABLE_PORT, &gpio);
    /* Keep every bridge branch disconnected until a mode route is applied. */
    BoardPanel_SetMuxEnabled(0U);
    return 0;
}

static uint8_t BoardPanel_BlinkOn(uint8_t level, uint32_t now_ms)
{
    uint32_t half_period;

    half_period = (level == 1U) ? 500U :
                  (level == 2U) ? 250U : 100U;
    return (((now_ms / half_period) & 1U) == 0U) ? 1U : 0U;
}

static uint8_t BoardPanel_BuzzerOn(uint8_t level, uint32_t now_ms)
{
    if (level == 1U) return ((now_ms % 2000U) < 250U) ? 1U : 0U;
    if (level == 2U) return ((now_ms % 500U) < 250U) ? 1U : 0U;
    if (level == 3U) return ((now_ms % 200U) < 100U) ? 1U : 0U;
    return 0U;
}

static int32_t BoardPanel_SetBuzzerOutput(uint8_t on)
{
    uint16_t dummy;
    uint8_t data[2];
    int32_t result;

    result = BoardPanel_WriteReg16(INA226_U10_ADDR7,
                                   INA226_REG_ALERT_LIMIT,
                                   (on != 0U) ? BUZZER_TRIGGER_LSB
                                              : BUZZER_SUPPRESS_LSB);
    if ((result == 0) && (on == 0U))
    {
        result = BSP_I2C_UI_ReadBytes(INA226_U10_ADDR7,
                                     INA226_REG_MASK_ENABLE,
                                     data, 2U);
        if (result == 0)
        {
            dummy = (uint16_t)(((uint16_t)data[0] << 8U) | data[1]);
            (void)dummy;
        }
    }
    return result;
}

static void BoardPanel_Update(void)
{
    static uint8_t previous_yds = 0xFFU;
    static uint8_t previous_buzzer = 0xFFU;
    BoardMonitorSnapshot monitor;
    uint32_t now_ms = HAL_GetTick();
    uint8_t current_level = 0U;
    uint8_t temperature_level = 0U;
    uint8_t alarm_level;
    uint8_t yds = 0U;
    uint8_t buzzer;
    uint8_t usb_active;
    int32_t result = 0;

    if (BoardMonitor_GetSnapshot(&monitor) == 0U)
    {
        return;
    }
    if (monitor.u10_valid != 0U) current_level = monitor.u10_level;
    if ((monitor.u44_valid != 0U) && (monitor.u44_level > current_level))
        current_level = monitor.u44_level;
    if (monitor.sht40_valid != 0U) temperature_level = monitor.temperature_level;
    alarm_level = (temperature_level > current_level) ?
                  temperature_level : current_level;

    if ((current_level != 0U) && BoardPanel_BlinkOn(current_level, now_ms))
        yds |= 0x01U;
    if ((monitor.u10_valid != 0U) && (monitor.u44_valid != 0U) &&
        (current_level == 0U))
        yds |= 0x02U;
    if ((monitor.sht40_valid != 0U) && (temperature_level == 0U))
        yds |= 0x04U;
    if ((temperature_level != 0U) &&
        BoardPanel_BlinkOn(temperature_level, now_ms))
        yds |= 0x08U;

    usb_active = ((board_panel_last_usb_ms != 0U) &&
                  ((now_ms - board_panel_last_usb_ms) < PANEL_USB_ACTIVE_MS))
                     ? 1U : 0U;
    if (usb_active != 0U) yds |= 0x10U;

    if (yds != previous_yds)
    {
        result = BoardPanel_WriteYds(yds);
        if (result == 0)
        {
            previous_yds = yds;
            board_panel_dbg.yds_write_count++;
        }
    }
    /* Level 1 is an early warning shown by YDS only.  Audible signalling is
     * reserved for alarm/emergency levels to avoid normal-load nuisance. */
    buzzer = ((board_panel_snapshot.buzzer_mute != 0U) ||
              (alarm_level < 2U)) ? 0U :
             BoardPanel_BuzzerOn(alarm_level, now_ms);
    if ((result == 0) && (buzzer != previous_buzzer))
    {
        result = BoardPanel_SetBuzzerOutput(buzzer);
        if (result == 0)
        {
            previous_buzzer = buzzer;
            board_panel_dbg.buzzer_write_count++;
        }
    }
    if (result != 0)
    {
        board_panel_dbg.write_error_count++;
    }

    BoardPanel_ReadPcalDiagnostics();

    taskENTER_CRITICAL();
    board_panel_snapshot.sequence++;
    board_panel_snapshot.timestamp_ms = now_ms;
    board_panel_snapshot.yds_bitmap = yds;
    board_panel_snapshot.alarm_level = alarm_level;
    board_panel_snapshot.usb_active = usb_active;
    board_panel_snapshot.valid = (result == 0) ? 1U : 0U;
    board_panel_dbg.update_count++;
    board_panel_dbg.last_result = result;
    board_panel_dbg.switch_bitmap = board_panel_snapshot.switch_bitmap;
    board_panel_dbg.yds_bitmap = yds;
    board_panel_dbg.buzzer_on = buzzer;
    board_panel_dbg.buzzer_mute = board_panel_snapshot.buzzer_mute;
    board_panel_dbg.alarm_level = alarm_level;
    taskEXIT_CRITICAL();
}

void BoardPanel_Init(void)
{
    memset((void *)&board_panel_dbg, 0, sizeof(board_panel_dbg));
    memset(&board_panel_snapshot, 0, sizeof(board_panel_snapshot));
    board_panel_last_usb_ms = 0U;
}

void BoardPanelTask(void const *argument)
{
    int32_t result;
    (void)argument;

    board_panel_dbg.started = 1U;
    /* INA226 survives an MCU-only reset.  Clear any alert limit/latch left
     * by the previous run before the lengthy FPGA/platform startup delay. */
    result = BoardPanel_SetBuzzerOutput(0U);
    if (result != 0)
    {
        board_panel_dbg.write_error_count++;
        board_panel_dbg.last_result = result;
    }
    osDelay(PANEL_STARTUP_DELAY_MS);
    do
    {
        result = BoardPanel_InitializeHardware();
        board_panel_dbg.last_result = result;
        if (result != 0)
        {
            board_panel_dbg.write_error_count++;
            osDelay(1000U);
        }
    } while (result != 0);
    board_panel_dbg.initialized = 1U;
    board_panel_snapshot.valid = 1U;
#if PANEL_FORCE_DOT_MATRIX_DIAG
    result = BoardPanel_ApplyModeRoute(PANEL_MODE_DOT_MATRIX);
    board_panel_dbg.last_result = result;
    if (result < 0)
    {
        board_panel_dbg.route_error_count++;
    }
#endif

    for (;;)
    {
        /* FPGA auto-configuration and the LCD serial pre-init share PH7 and
         * may leave it high.  Reassert the selected runtime route so M11's
         * active-low U4-U7 enable cannot silently become disconnected. */
        BoardPanel_SetMuxEnabled(
            (board_panel_dbg.routed_mode == PANEL_MODE_DOT_MATRIX) ? 1U : 0U);
        BoardPanel_Update();
        osDelay(PANEL_UPDATE_MS);
    }
}

uint8_t BoardPanel_GetSnapshot(BoardPanelSnapshot *snapshot)
{
    if (snapshot == NULL) return 0U;
    taskENTER_CRITICAL();
    *snapshot = board_panel_snapshot;
    taskEXIT_CRITICAL();
    return board_panel_dbg.initialized;
}

int32_t BoardPanel_SetSwitch(uint8_t b_number, uint8_t on,
                             uint32_t command_id)
{
    uint16_t next;
    int32_t result;

    if ((b_number < 1U) || (b_number > 14U) || (b_number == 10U) ||
        (on > 1U) || (command_id == 0U) ||
        (board_panel_dbg.initialized == 0U))
        return -1;
    if (command_id == board_panel_snapshot.control_ack) return 1;

    taskENTER_CRITICAL();
    next = board_panel_snapshot.switch_bitmap;
    taskEXIT_CRITICAL();
    if (on != 0U) next |= (uint16_t)(1U << (b_number - 1U));
    else next &= (uint16_t)~(1U << (b_number - 1U));
    next = BoardPanel_NormalizeSwitches(next);
    result = BoardPanel_WriteSwitches(next);
    if (result != 0)
    {
        board_panel_dbg.write_error_count++;
        board_panel_dbg.last_result = result;
        return -2;
    }
    taskENTER_CRITICAL();
    board_panel_snapshot.switch_bitmap = next;
    board_panel_snapshot.control_ack = command_id;
    board_panel_dbg.last_control_id = command_id;
    board_panel_dbg.switch_write_count++;
    board_panel_dbg.switch_bitmap = next;
    taskEXIT_CRITICAL();
    return 0;
}

int32_t BoardPanel_SetBuzzerMute(uint8_t mute, uint32_t command_id)
{
    if ((mute > 1U) || (command_id == 0U) ||
        (board_panel_dbg.initialized == 0U))
        return -1;
    if (command_id == board_panel_snapshot.control_ack) return 1;
    taskENTER_CRITICAL();
    board_panel_snapshot.buzzer_mute = mute;
    board_panel_snapshot.control_ack = command_id;
    board_panel_dbg.last_control_id = command_id;
    board_panel_dbg.buzzer_mute = mute;
    taskEXIT_CRITICAL();
    return 0;
}

int32_t BoardPanel_SetMode(uint8_t mode, uint32_t command_id)
{
    uint8_t previous_mode;
    int32_t result;

    if (((mode != PANEL_MODE_COUNTER) &&
         (mode != PANEL_MODE_DOT_MATRIX)) ||
        (command_id == 0U))
        return -1;
    if (command_id == board_panel_snapshot.control_ack) return 1;

    previous_mode = board_panel_snapshot.mode;
    result = BoardPanel_ApplyModeRoute(mode);
    if (result < 0)
        return -2;
    result = M0_DataSource_SetMode(mode);
    if (result != 0)
    {
        (void)BoardPanel_ApplyModeRoute(previous_mode);
        board_panel_dbg.write_error_count++;
        board_panel_dbg.last_result = result;
        return -3;
    }
    taskENTER_CRITICAL();
    board_panel_snapshot.mode = mode;
    board_panel_snapshot.control_ack = command_id;
    board_panel_dbg.last_control_id = command_id;
    board_panel_dbg.mode_write_count++;
    board_panel_dbg.mode = mode;
    taskEXIT_CRITICAL();
    return 0;
}

int32_t BoardPanel_ApplyModeRoute(uint8_t mode)
{
    uint16_t desired;
    uint16_t current;
    int32_t result;

    if ((mode != PANEL_MODE_COUNTER) &&
        (mode != PANEL_MODE_DOT_MATRIX))
        return -1;
    if (board_panel_dbg.initialized == 0U)
        return -2;

    desired = (mode == PANEL_MODE_DOT_MATRIX) ?
              PANEL_ROUTE_DOT_MATRIX : PANEL_ROUTE_ALL_OFF;
    taskENTER_CRITICAL();
    current = board_panel_snapshot.switch_bitmap;
    taskEXIT_CRITICAL();
    if ((current == desired) && (board_panel_dbg.routed_mode == mode))
    {
        BoardPanel_SetMuxEnabled(
            (mode == PANEL_MODE_DOT_MATRIX) ? 1U : 0U);
        return 1;
    }

    /* Break before make: disable every BSW group before selecting A-B3. */
    BoardPanel_SetMuxEnabled(0U);
    if (current != PANEL_ROUTE_ALL_OFF)
    {
        result = BoardPanel_WriteSwitches(PANEL_ROUTE_ALL_OFF);
        if (result != 0)
        {
            board_panel_dbg.route_error_count++;
            board_panel_dbg.last_result = result;
            return -3;
        }
    }
    if (desired != PANEL_ROUTE_ALL_OFF)
    {
        result = BoardPanel_WriteSwitches(desired);
        if (result != 0)
        {
            (void)BoardPanel_WriteSwitches(PANEL_ROUTE_ALL_OFF);
            board_panel_dbg.route_error_count++;
            board_panel_dbg.last_result = result;
            return -4;
        }
    }

    BoardPanel_SetMuxEnabled(
        (mode == PANEL_MODE_DOT_MATRIX) ? 1U : 0U);

    taskENTER_CRITICAL();
    board_panel_snapshot.switch_bitmap = desired;
    board_panel_snapshot.mode = mode;
    board_panel_dbg.switch_bitmap = desired;
    board_panel_dbg.routed_mode = mode;
    board_panel_dbg.route_write_count++;
    taskEXIT_CRITICAL();
    return 0;
}

void BoardPanel_NotifyUsbActivity(void)
{
    board_panel_last_usb_ms = HAL_GetTick();
}

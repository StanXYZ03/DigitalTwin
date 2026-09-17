#include "board_monitor.h"

#include "bsp_i2c_ui.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stm32h7xx_hal.h"
#include <string.h>

#define BOARD_MONITOR_STARTUP_DELAY_MS  8000U
#define BOARD_MONITOR_PERIOD_MS         1000U
#define BOARD_MONITOR_FAIL_INVALID      3U

#define SHT40_ADDR7                     0x44U
#define SHT40_MEASURE_HIGH_CMD          0xFDU
#define SHT40_MEASURE_DELAY_MS          15U

#define INA226_U10_ADDR7                0x41U
#define INA226_U44_ADDR7                0x40U
#define INA226_REG_CONFIG               0x00U
#define INA226_REG_SHUNT                0x01U
#define INA226_CONTINUOUS_CONFIG        0x0527U

#define U10_WARN_LSB                    1190
#define U10_ALARM_LSB                   1400
#define U10_EMERGENCY_LSB               2100
/* U44 uses a 20 mOhm shunt: one raw shunt LSB represents 0.125 mA.
 * Use 400 mA as the audible alarm threshold, with an 85% early warning
 * and a 150% emergency threshold. */
#define U44_WARN_LSB                    2720  /* 340 mA */
#define U44_ALARM_LSB                   3200  /* 400 mA */
#define U44_EMERGENCY_LSB               4800  /* 600 mA */

#define TEMP_WARN_CENTI                 5800
#define TEMP_ALARM_CENTI                6000
#define TEMP_EMERGENCY_CENTI            6500

volatile BoardMonitorDebug board_monitor_dbg;
static BoardMonitorSnapshot board_monitor_snapshot;
static uint8_t board_monitor_have_snapshot;

static uint8_t BoardMonitor_Crc8(const uint8_t *data, uint32_t length)
{
    uint32_t index;
    uint32_t bit;
    uint8_t crc = 0xFFU;

    for (index = 0U; index < length; ++index)
    {
        crc ^= data[index];
        for (bit = 0U; bit < 8U; ++bit)
        {
            crc = ((crc & 0x80U) != 0U)
                      ? (uint8_t)((crc << 1U) ^ 0x31U)
                      : (uint8_t)(crc << 1U);
        }
    }
    return crc;
}

static uint8_t BoardMonitor_Level(int32_t value, int32_t warn,
                                  int32_t alarm, int32_t emergency)
{
    if (value > emergency)
    {
        return 3U;
    }
    if (value > alarm)
    {
        return 2U;
    }
    if (value > warn)
    {
        return 1U;
    }
    return 0U;
}

static int32_t BoardMonitor_ReadSht40(BoardMonitorSnapshot *snapshot)
{
    uint8_t command = SHT40_MEASURE_HIGH_CMD;
    uint8_t data[6];
    uint16_t raw_temperature;
    uint16_t raw_humidity;
    int32_t temperature_centi;
    int32_t humidity_deci;

    if (BSP_I2C_UI_WriteRaw(SHT40_ADDR7, &command, 1U) != 0)
    {
        return -1;
    }
    osDelay(SHT40_MEASURE_DELAY_MS);
    if (BSP_I2C_UI_ReadRaw(SHT40_ADDR7, data, sizeof(data)) != 0)
    {
        return -2;
    }
    if ((BoardMonitor_Crc8(&data[0], 2U) != data[2]) ||
        (BoardMonitor_Crc8(&data[3], 2U) != data[5]))
    {
        return -3;
    }

    raw_temperature = (uint16_t)(((uint16_t)data[0] << 8U) | data[1]);
    raw_humidity = (uint16_t)(((uint16_t)data[3] << 8U) | data[4]);
    temperature_centi = -4500 +
        (int32_t)(((int64_t)17500 * raw_temperature + 32767) / 65535);
    humidity_deci = -60 +
        (int32_t)(((int64_t)1250 * raw_humidity + 32767) / 65535);
    if (humidity_deci < 0)
    {
        humidity_deci = 0;
    }
    else if (humidity_deci > 1000)
    {
        humidity_deci = 1000;
    }

    board_monitor_dbg.sht40_raw_temperature = raw_temperature;
    board_monitor_dbg.sht40_raw_humidity = raw_humidity;
    snapshot->temperature_deci_c = (temperature_centi >= 0)
        ? ((temperature_centi + 5) / 10)
        : ((temperature_centi - 5) / 10);
    snapshot->humidity_deci_percent = (uint16_t)humidity_deci;
    snapshot->temperature_level = BoardMonitor_Level(
        temperature_centi, TEMP_WARN_CENTI, TEMP_ALARM_CENTI,
        TEMP_EMERGENCY_CENTI);
    return 0;
}

static int32_t BoardMonitor_WriteIna226Reg16(uint8_t address7,
                                             uint8_t reg,
                                             uint16_t value)
{
    uint8_t data[2];

    data[0] = (uint8_t)(value >> 8U);
    data[1] = (uint8_t)value;
    return BSP_I2C_UI_WriteBytes(address7, reg, data, 2U);
}

static int32_t BoardMonitor_ReadIna226Reg16(uint8_t address7,
                                            uint8_t reg,
                                            uint16_t *value)
{
    uint8_t data[2];

    if ((value == NULL) ||
        (BSP_I2C_UI_ReadBytes(address7, reg, data, 2U) != 0))
    {
        return -1;
    }
    *value = (uint16_t)(((uint16_t)data[0] << 8U) | data[1]);
    return 0;
}

static int32_t BoardMonitor_ReadIna226(uint8_t address7,
                                       volatile uint8_t *configured,
                                       int16_t *raw_shunt)
{
    uint16_t value;

    if (*configured == 0U)
    {
        if (BoardMonitor_WriteIna226Reg16(address7, INA226_REG_CONFIG,
                                          INA226_CONTINUOUS_CONFIG) != 0)
        {
            return -1;
        }
        *configured = 1U;
    }
    if (BoardMonitor_ReadIna226Reg16(address7, INA226_REG_SHUNT,
                                     &value) != 0)
    {
        *configured = 0U;
        return -2;
    }
    *raw_shunt = (int16_t)value;
    return 0;
}

static void BoardMonitor_RecordFailure(volatile uint8_t *streak,
                                       uint8_t *valid)
{
    if (*streak < 0xFFU)
    {
        (*streak)++;
    }
    if (*streak >= BOARD_MONITOR_FAIL_INVALID)
    {
        *valid = 0U;
    }
}

static void BoardMonitor_Sample(void)
{
    static const uint8_t known_addresses[7] = {
        0x22U, 0x26U, 0x27U, 0x40U, 0x41U, 0x44U, 0x60U
    };
    BoardMonitorSnapshot next;
    int32_t result;
    int16_t raw_shunt;
    uint32_t index;

    taskENTER_CRITICAL();
    next = board_monitor_snapshot;
    taskEXIT_CRITICAL();
    next.sequence++;
    next.timestamp_ms = HAL_GetTick();

    if (board_monitor_dbg.sample_count == 0U)
    {
        board_monitor_dbg.probe_mask_known = 0U;
        board_monitor_dbg.probe_mask_40_47 = 0U;
        for (index = 0U; index < 7U; ++index)
        {
            if (BSP_I2C_UI_Probe(known_addresses[index]) == 0)
            {
                board_monitor_dbg.probe_mask_known |=
                    (uint8_t)(1U << index);
            }
        }
        for (index = 0x40U; index <= 0x47U; ++index)
        {
            if (BSP_I2C_UI_Probe((uint8_t)index) == 0)
            {
                board_monitor_dbg.probe_mask_40_47 |=
                    (uint8_t)(1U << (index - 0x40U));
            }
        }
    }

    result = BoardMonitor_ReadSht40(&next);
    board_monitor_dbg.sht40_last_result = result;
    if (result == 0)
    {
        next.sht40_valid = 1U;
        board_monitor_dbg.sht40_fail_streak = 0U;
        board_monitor_dbg.sht40_success_count++;
    }
    else
    {
        BoardMonitor_RecordFailure(
            &board_monitor_dbg.sht40_fail_streak,
            &next.sht40_valid);
        board_monitor_dbg.sht40_error_count++;
    }

    raw_shunt = 0;
    result = BoardMonitor_ReadIna226(INA226_U10_ADDR7,
        &board_monitor_dbg.u10_configured, &raw_shunt);
    board_monitor_dbg.u10_last_result = result;
    if (result == 0)
    {
        board_monitor_dbg.u10_raw_shunt = raw_shunt;
        next.u10_current_ma = (int32_t)raw_shunt / 40;
        next.u10_level = BoardMonitor_Level(raw_shunt, U10_WARN_LSB,
                                            U10_ALARM_LSB,
                                            U10_EMERGENCY_LSB);
        next.u10_valid = 1U;
        board_monitor_dbg.u10_fail_streak = 0U;
        board_monitor_dbg.u10_success_count++;
    }
    else
    {
        BoardMonitor_RecordFailure(
            &board_monitor_dbg.u10_fail_streak,
            &next.u10_valid);
        board_monitor_dbg.u10_error_count++;
    }

    raw_shunt = 0;
    result = BoardMonitor_ReadIna226(INA226_U44_ADDR7,
        &board_monitor_dbg.u44_configured, &raw_shunt);
    board_monitor_dbg.u44_last_result = result;
    if (result == 0)
    {
        board_monitor_dbg.u44_raw_shunt = raw_shunt;
        next.u44_current_ma = (int32_t)raw_shunt / 8;
        next.u44_level = BoardMonitor_Level(raw_shunt, U44_WARN_LSB,
                                            U44_ALARM_LSB,
                                            U44_EMERGENCY_LSB);
        next.u44_valid = 1U;
        board_monitor_dbg.u44_fail_streak = 0U;
        board_monitor_dbg.u44_success_count++;
    }
    else
    {
        BoardMonitor_RecordFailure(
            &board_monitor_dbg.u44_fail_streak,
            &next.u44_valid);
        board_monitor_dbg.u44_error_count++;
    }

    taskENTER_CRITICAL();
    board_monitor_snapshot = next;
    board_monitor_have_snapshot = 1U;
    taskEXIT_CRITICAL();
    board_monitor_dbg.sample_count++;
}

void BoardMonitor_Init(void)
{
    uint32_t address;

    memset((void *)&board_monitor_dbg, 0, sizeof(board_monitor_dbg));
    memset(&board_monitor_snapshot, 0, sizeof(board_monitor_snapshot));
    board_monitor_have_snapshot = 0U;
    BSP_I2C_UI_Init();
    board_monitor_dbg.pca9617_enabled = 1U;
    for (address = 0x40U; address <= 0x47U; ++address)
    {
        if (BSP_I2C_UI_Probe((uint8_t)address) == 0)
        {
            board_monitor_dbg.probe_mask_40_47 |=
                (uint8_t)(1U << (address - 0x40U));
        }
    }
}

void BoardMonitorTask(void const *argument)
{
    (void)argument;
    board_monitor_dbg.started = 1U;
    osDelay(BOARD_MONITOR_STARTUP_DELAY_MS);
    for (;;)
    {
        BoardMonitor_Sample();
        osDelay(BOARD_MONITOR_PERIOD_MS);
    }
}

uint8_t BoardMonitor_GetSnapshot(BoardMonitorSnapshot *snapshot)
{
    uint8_t available;

    if (snapshot == NULL)
    {
        return 0U;
    }
    taskENTER_CRITICAL();
    available = board_monitor_have_snapshot;
    if (available != 0U)
    {
        *snapshot = board_monitor_snapshot;
    }
    taskEXIT_CRITICAL();
    return available;
}

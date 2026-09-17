#ifndef BOARD_MONITOR_H
#define BOARD_MONITOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct
{
    uint32_t sequence;
    uint32_t timestamp_ms;
    int32_t temperature_deci_c;
    uint16_t humidity_deci_percent;
    int32_t u10_current_ma;
    int32_t u44_current_ma;
    uint8_t sht40_valid;
    uint8_t u10_valid;
    uint8_t u44_valid;
    uint8_t temperature_level;
    uint8_t u10_level;
    uint8_t u44_level;
} BoardMonitorSnapshot;

typedef struct
{
    uint32_t started;
    uint32_t sample_count;
    uint32_t sht40_success_count;
    uint32_t sht40_error_count;
    uint32_t u10_success_count;
    uint32_t u10_error_count;
    uint32_t u44_success_count;
    uint32_t u44_error_count;
    int32_t sht40_last_result;
    int32_t u10_last_result;
    int32_t u44_last_result;
    uint16_t sht40_raw_temperature;
    uint16_t sht40_raw_humidity;
    int16_t u10_raw_shunt;
    int16_t u44_raw_shunt;
    uint8_t sht40_fail_streak;
    uint8_t u10_fail_streak;
    uint8_t u44_fail_streak;
    uint8_t u10_configured;
    uint8_t u44_configured;
    uint8_t probe_mask_40_47;
    uint8_t probe_mask_known;
    uint8_t pca9617_enabled;
} BoardMonitorDebug;

extern volatile BoardMonitorDebug board_monitor_dbg;

void BoardMonitor_Init(void);
void BoardMonitorTask(void const *argument);
uint8_t BoardMonitor_GetSnapshot(BoardMonitorSnapshot *snapshot);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_MONITOR_H */

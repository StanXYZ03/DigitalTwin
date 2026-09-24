#ifndef BOARD_PANEL_H
#define BOARD_PANEL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef struct
{
    uint32_t sequence;
    uint32_t timestamp_ms;
    uint32_t control_ack;
    uint16_t switch_bitmap;
    uint8_t yds_bitmap;
    uint8_t buzzer_mute;
    uint8_t alarm_level;
    uint8_t usb_active;
    uint8_t valid;
    uint8_t mode;
} BoardPanelSnapshot;

typedef struct
{
    uint32_t started;
    uint32_t update_count;
    uint32_t write_error_count;
    uint32_t switch_write_count;
    uint32_t yds_write_count;
    uint32_t buzzer_write_count;
    uint32_t mode_write_count;
    uint32_t route_write_count;
    uint32_t route_error_count;
    uint32_t pcal_read_count;
    uint32_t pcal_read_error_count;
    uint32_t last_control_id;
    int32_t last_result;
    uint16_t switch_bitmap;
    uint8_t yds_bitmap;
    uint8_t buzzer_on;
    uint8_t buzzer_mute;
    uint8_t alarm_level;
    uint8_t initialized;
    uint8_t mode;
    uint8_t routed_mode;
    uint8_t pcal_in0;
    uint8_t pcal_in1;
    uint8_t pcal_in2;
    uint8_t pcal_out0;
    uint8_t pcal_out1;
    uint8_t pcal_out2;
    uint8_t mux_enable_ph7;
    int32_t pcal_read_result;
} BoardPanelDebug;

extern volatile BoardPanelDebug board_panel_dbg;

void BoardPanel_Init(void);
void BoardPanelTask(void const *argument);
uint8_t BoardPanel_GetSnapshot(BoardPanelSnapshot *snapshot);
int32_t BoardPanel_SetSwitch(uint8_t b_number, uint8_t on,
                             uint32_t command_id);
int32_t BoardPanel_SetBuzzerMute(uint8_t mute, uint32_t command_id);
int32_t BoardPanel_SetMode(uint8_t mode, uint32_t command_id);
int32_t BoardPanel_AcknowledgeSystemReset(uint32_t command_id);
int32_t BoardPanel_ApplyModeRoute(uint8_t mode);
void BoardPanel_NotifyUsbActivity(void);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_PANEL_H */

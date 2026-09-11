#ifndef M0_DATA_SOURCE_H
#define M0_DATA_SOURCE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* M0 data received from the Lattice FMC16 GET_STATUS_SNAPSHOT response. */
typedef struct
{
    uint32_t sequence;
    uint32_t timestamp_ms;
    uint32_t po;
    uint32_t clock_hz;
    uint16_t pio;
    uint16_t pi_applied;
    uint16_t pi_requested;
    uint16_t key_state;
    uint16_t key_event_count;
    uint16_t pi_virtual_toggle;
    uint16_t virtual_key_state;
    uint16_t virtual_key_count;
    uint32_t control_ack;
    uint8_t mode;
    uint8_t clk_sel;
} M0_DataSnapshot;

typedef enum
{
    M0_FMC_OK = 0,
    M0_FMC_ERR_ARGUMENT = -1,
    M0_FMC_ERR_TIMEOUT = -2,
    M0_FMC_ERR_LENGTH = -3,
    M0_FMC_ERR_VERSION = -4,
    M0_FMC_ERR_HEADER_CRC = -5,
    M0_FMC_ERR_PACKET_CRC = -6,
    M0_FMC_ERR_RESPONSE = -7,
    M0_FMC_ERR_VALIDITY = -8
} M0_FmcResult;

typedef struct
{
    uint32_t initialized;
    uint32_t read_attempts;
    uint32_t read_successes;
    uint32_t read_failures;
    uint32_t timeout_count;
    uint32_t header_crc_error_count;
    uint32_t packet_crc_error_count;
    uint32_t length_error_count;
    uint32_t ignored_packets;
    uint32_t rx_word_count;
    uint32_t tx_word_count;
    uint32_t command_sequence;
    uint32_t transaction_id;
    uint32_t last_packet_transaction;
    uint32_t response_sequence;
    uint32_t lattice_timestamp_ms;
    uint32_t po;
    int32_t last_result;
    uint16_t last_raw_word;
    uint16_t last_packet_words;
    uint16_t last_packet_type;
    uint16_t response_status;
    uint16_t response_detail;
    uint16_t schema;
    uint16_t validity;
    uint16_t pio;
    uint16_t pi_requested;
    uint16_t pi_applied;
    uint16_t key_state;
    uint16_t key_event_count;
    uint16_t pi_i2c_error_count;
    uint16_t pi_virtual_toggle;
    uint16_t virtual_key_state;
    uint16_t virtual_key_count;
    uint32_t last_control_id;
    uint32_t control_duplicate_count;
    uint32_t control_reject_count;
    uint32_t fmc_bcr1;
    uint32_t fmc_btr1;
    uint32_t fmc_bwtr1;
    uint32_t ltdc_error_callback_count;
    uint32_t ltdc_error_code;
    uint32_t bridge_enable_count;
    uint32_t bridge_gpioh_odr;
    uint32_t bridge_settle_cycles;
} M0_FmcDebug;

extern volatile M0_FmcDebug m0_fmc_dbg;

void M0_DataSource_Init(void);
uint8_t M0_DataSource_Read(M0_DataSnapshot *snapshot);
int32_t M0_DataSource_VirtualKeySet(uint8_t f_number,
                                   uint8_t pressed,
                                   uint32_t command_id);

#ifdef __cplusplus
}
#endif

#endif /* M0_DATA_SOURCE_H */

#ifndef FMC16_LINK_TEST_DEBUG_H
#define FMC16_LINK_TEST_DEBUG_H

#include <stdint.h>

typedef struct
{
    uint32_t magic;
    uint32_t run_count;
    int32_t smoke_result;
    int32_t port_result;
    int32_t protocol_result;
    uint32_t stage;
    uint32_t peer_capabilities;
    uint32_t rx_word_count;
    uint32_t tx_word_count;
    uint32_t packet_sequence;
    uint32_t packet_transaction;
    uint32_t fmc_bcr1;
    uint32_t fmc_btr1;
    uint32_t fmc_bwtr1;
    uint32_t bridge_odr_before;
    uint32_t bridge_odr_enabled;
    uint32_t bridge_odr_after;
    uint16_t last_raw_word;
    uint16_t packet_words;
    uint16_t packet_type;
    uint16_t packet_flags;
    uint32_t sysclk_hz;
    uint32_t fmcclk_hz;
    uint32_t rcc_d1cfgr;
    uint32_t rcc_pll2divr;
} FMC16_LinkTestDebug;

extern volatile FMC16_LinkTestDebug fmc16_link_test_dbg;

#endif

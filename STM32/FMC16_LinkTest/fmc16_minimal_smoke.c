#include "fmc16_port_stm32h743.h"
#include "fmc16_protocol.h"
#include "fmc16_smoke.h"
#include "fmc16_link_test_debug.h"

#define FMC16_CAPTURE_LIMIT 500000UL

typedef enum {
    FMC16_SMOKE_OK = 0,
    FMC16_SMOKE_PORT = -200,
    FMC16_SMOKE_RESET_NOTICE = -201,
    FMC16_SMOKE_LINK_HELLO = -202,
    FMC16_SMOKE_DEVICE_INFO = -203,
    FMC16_SMOKE_COMMAND = -204,
    FMC16_SMOKE_ACK = -205,
    FMC16_SMOKE_RESPONSE = -206
} fmc16_smoke_result_t;

static uint16_t packet[FMC16_MAX_PACKET_WORDS];

static fmc16_result_t capture_any(size_t *captured_words)
{
    size_t words = 0U;
    fmc16_result_t result;

    result = fmc16_capture_packet(fmc16_stm32h743_read_word, NULL,
                                  packet, FMC16_MAX_PACKET_WORDS,
                                  FMC16_CAPTURE_LIMIT, &words);
    fmc16_link_test_dbg.protocol_result = result;
    fmc16_link_test_dbg.packet_words = (uint16_t)words;
    if (result != FMC16_OK) {
        *captured_words = words;
        return result;
    }
    fmc16_link_test_dbg.packet_type = fmc16_packet_type(packet);
    fmc16_link_test_dbg.packet_flags = fmc16_packet_flags(packet);
    fmc16_link_test_dbg.packet_sequence = fmc16_packet_sequence(packet);
    fmc16_link_test_dbg.packet_transaction =
        fmc16_packet_transaction(packet);
    *captured_words = words;
    return FMC16_OK;
}

int fmc16_minimal_smoke_run_with_capabilities(uint32_t *peer_capabilities)
{
    const uint32_t transaction_id = 0x13572468UL;
    uint32_t hello_capabilities;
    uint32_t response_capabilities;
    uint32_t boot_index;
    uint8_t packet_type;
    size_t words = 0U;
    fmc16_result_t result;

    if (peer_capabilities != NULL) {
        *peer_capabilities = 0U;
    }

    fmc16_link_test_dbg.stage = 2U;
    fmc16_stm32h743_mpu_config();
    fmc16_link_test_dbg.port_result = fmc16_stm32h743_init();
    if (fmc16_link_test_dbg.port_result != FMC16_STM32_OK) {
        return FMC16_SMOKE_PORT;
    }

    /* A flash download starts the STM32 once before a debugger can attach,
     * so some or all one-shot XO2 boot packets may already be consumed.
     * Drain and validate any boot packets still queued, then always exercise
     * the repeatable command/ACK/response path. */
    hello_capabilities = 0U;
    fmc16_link_test_dbg.stage = 10U;
    for (boot_index = 0U; boot_index < 3U; ++boot_index) {
        result = capture_any(&words);
        if (result != FMC16_OK) {
            break;
        }
        packet_type = fmc16_packet_type(packet);
        if (packet_type == FMC16_TYPE_RESET_NOTICE) {
            if ((words != 23U) || (fmc16_packet_sequence(packet) != 0U)) {
                return FMC16_SMOKE_RESET_NOTICE;
            }
        } else if (packet_type == FMC16_TYPE_LINK_HELLO) {
            hello_capabilities = ((uint32_t)packet[17] << 16) | packet[18];
            if ((words != 26U) || (fmc16_packet_sequence(packet) != 1U) ||
                (packet[13] != 0x584FU) ||
                !fmc16_capabilities_compatible(hello_capabilities)) {
                return FMC16_SMOKE_LINK_HELLO;
            }
        } else if (packet_type == FMC16_TYPE_DEVICE_INFO) {
            if ((words != 31U) || (fmc16_packet_sequence(packet) != 2U)) {
                return FMC16_SMOKE_DEVICE_INFO;
            }
        }
    }

    fmc16_link_test_dbg.stage = 20U;
    result = fmc16_build_get_capabilities(packet, FMC16_MAX_PACKET_WORDS,
                                          1U, transaction_id,
                                          FMC16_FLAG_ACK_REQUIRED, &words);
    if ((result != FMC16_OK) ||
        (fmc16_write_packet(fmc16_stm32h743_write_word, NULL,
                            packet, words) != FMC16_OK)) {
        return FMC16_SMOKE_COMMAND;
    }

    fmc16_link_test_dbg.stage = 30U;
    result = fmc16_capture_packet(fmc16_stm32h743_read_word, NULL,
                                  packet, FMC16_MAX_PACKET_WORDS,
                                  FMC16_CAPTURE_LIMIT, &words);
    fmc16_link_test_dbg.protocol_result = result;
    fmc16_link_test_dbg.packet_words = (uint16_t)words;
    if ((result != FMC16_OK) || (words != 19U) ||
        (fmc16_packet_type(packet) != FMC16_TYPE_ACK) ||
        (fmc16_packet_transaction(packet) != transaction_id) ||
        (packet[13] != FMC16_TYPE_COMMAND) ||
        (packet[14] != FMC16_STATUS_ACCEPTED) ||
        (packet[15] != FMC16_OPCODE_GET_CAPABILITIES)) {
        return FMC16_SMOKE_ACK;
    }

    fmc16_link_test_dbg.packet_type = fmc16_packet_type(packet);
    fmc16_link_test_dbg.packet_transaction =
        fmc16_packet_transaction(packet);
    fmc16_link_test_dbg.stage = 40U;
    result = fmc16_capture_packet(fmc16_stm32h743_read_word, NULL,
                                  packet, FMC16_MAX_PACKET_WORDS,
                                  FMC16_CAPTURE_LIMIT, &words);
    fmc16_link_test_dbg.protocol_result = result;
    fmc16_link_test_dbg.packet_words = (uint16_t)words;
    if ((result != FMC16_OK) || (words != 25U)) {
        return FMC16_SMOKE_RESPONSE;
    }
    response_capabilities = ((uint32_t)packet[17] << 16) | packet[18];
    fmc16_link_test_dbg.packet_type = fmc16_packet_type(packet);
    fmc16_link_test_dbg.packet_flags = fmc16_packet_flags(packet);
    fmc16_link_test_dbg.packet_sequence = fmc16_packet_sequence(packet);
    fmc16_link_test_dbg.packet_transaction =
        fmc16_packet_transaction(packet);
    if ((fmc16_packet_type(packet) != FMC16_TYPE_RESPONSE) ||
        (fmc16_packet_transaction(packet) != transaction_id) ||
        (packet[13] != FMC16_OPCODE_GET_CAPABILITIES) ||
        (packet[14] != FMC16_STATUS_OK) ||
        (packet[16] != 6U) ||
        !fmc16_capabilities_compatible(response_capabilities) ||
        ((hello_capabilities != 0U) &&
         (response_capabilities != hello_capabilities))) {
        return FMC16_SMOKE_RESPONSE;
    }
    if (peer_capabilities != NULL) {
        *peer_capabilities = response_capabilities;
    }
    return FMC16_SMOKE_OK;
}

int fmc16_minimal_smoke_run(void)
{
    return fmc16_minimal_smoke_run_with_capabilities(NULL);
}

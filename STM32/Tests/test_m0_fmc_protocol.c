#define M0_FMC_PROTOCOL_TEST 1
#include "../Core/Src/m0_data_source.c"

#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) return __LINE__; } while (0)

static size_t make_xo2_packet(uint16_t *packet,
                              uint8_t type,
                              uint16_t channel,
                              const uint16_t *payload,
                              size_t payload_words,
                              uint32_t sequence,
                              uint32_t transaction)
{
    size_t total;
    size_t i;
    uint32_t crc32;

    total = FMC16_MIN_PACKET_WORDS + payload_words;
    packet[0] = FMC16_SOF0;
    packet[1] = FMC16_SOF1;
    packet[2] = FMC16_VERSION_HEADER;
    packet[3] = (uint16_t)total;
    packet[4] = (uint16_t)((uint16_t)type << 8);
    packet[5] = 0x0100U;
    packet[6] = channel;
    packet[7] = (uint16_t)payload_words;
    packet[8] = (uint16_t)(sequence >> 16);
    packet[9] = (uint16_t)sequence;
    packet[10] = (uint16_t)(transaction >> 16);
    packet[11] = (uint16_t)transaction;
    packet[12] = fmc16_crc16_words(&packet[2], 10U);
    for (i = 0U; i < payload_words; ++i)
    {
        packet[13U + i] = payload[i];
    }
    crc32 = fmc16_crc32_words(&packet[2], total - 4U);
    packet[total - 2U] = (uint16_t)(crc32 >> 16);
    packet[total - 1U] = (uint16_t)crc32;
    return total;
}

static int test_command_builder(void)
{
    uint16_t packet[FMC16_COMMAND_WORDS];

    fmc16_build_status_command(packet, 0x11223344UL, 0x55667788UL);
    CHECK(fmc16_validate_packet(packet, FMC16_COMMAND_WORDS) == M0_FMC_OK);
    CHECK(packet[3] == 19U);
    CHECK(packet[4] == 0x3000U);
    CHECK(packet[5] == 0x0001U);
    CHECK(packet[6] == FMC16_CHANNEL_COMMAND);
    CHECK(packet[7] == 4U);
    CHECK(packet[8] == 0x1122U && packet[9] == 0x3344U);
    CHECK(packet[10] == 0x5566U && packet[11] == 0x7788U);
    CHECK(packet[13] == FMC16_OPCODE_GET_STATUS);
    CHECK(packet[14] == FMC16_SCHEMA_V1_0);
    CHECK(packet[15] == 0U && packet[16] == 0U);

    packet[15] ^= 1U;
    CHECK(fmc16_validate_packet(packet, FMC16_COMMAND_WORDS) ==
          M0_FMC_ERR_PACKET_CRC);
    packet[15] ^= 1U;
    packet[8] ^= 1U;
    CHECK(fmc16_validate_packet(packet, FMC16_COMMAND_WORDS) ==
          M0_FMC_ERR_HEADER_CRC);
    return 0;
}

static int test_status_response(void)
{
    uint16_t packet[FMC16_MAX_PACKET_WORDS];
    const uint16_t payload[14] = {
        FMC16_OPCODE_GET_STATUS, FMC16_STATUS_OK, 0x0000U, 10U,
        FMC16_SCHEMA_V1_0, 0x0306U, 0x1F00U, 0x0005U, 0x002AU,
        0x1122U, 0x3344U, 0x55AAU, 0x0009U, 0xC4D2U
    };
    M0_DataSnapshot snapshot;
    size_t words;

    memset(&snapshot, 0, sizeof(snapshot));
    words = make_xo2_packet(packet, FMC16_TYPE_RESPONSE,
                            FMC16_CHANNEL_COMMAND, payload, 14U,
                            0x01020304UL, 0x55667788UL);
    CHECK(words == FMC16_RESPONSE_WORDS);
    CHECK(fmc16_parse_status_response(packet, words, 0x55667788UL,
                                      &snapshot) == M0_FMC_OK);
    CHECK(snapshot.sequence == 0x01020304UL);
    CHECK(snapshot.po == 0x11223344UL);
    CHECK(snapshot.pio == 0x55AAU);
    CHECK(snapshot.pi_requested == 0x1F00U);
    CHECK(snapshot.key_state == 0x0005U);
    CHECK(snapshot.key_event_count == 0x002AU);
    CHECK(snapshot.pi_applied == 0U);
    CHECK(snapshot.timestamp_ms == 0x0009C4D2UL);

    CHECK(fmc16_parse_status_response(packet, words, 0x55667789UL,
                                      &snapshot) == M0_FMC_ERR_RESPONSE);

    packet[18] = 0x0002U;
    packet[12] = fmc16_crc16_words(&packet[2], 10U);
    {
        uint32_t crc32;
        crc32 = fmc16_crc32_words(&packet[2], words - 4U);
        packet[words - 2U] = (uint16_t)(crc32 >> 16);
        packet[words - 1U] = (uint16_t)crc32;
    }
    CHECK(fmc16_parse_status_response(packet, words, 0x55667788UL,
                                      &snapshot) == M0_FMC_ERR_VALIDITY);

    packet[18] = 0x0406U;
    {
        uint32_t crc32;
        crc32 = fmc16_crc32_words(&packet[2], words - 4U);
        packet[words - 2U] = (uint16_t)(crc32 >> 16);
        packet[words - 1U] = (uint16_t)crc32;
    }
    CHECK(fmc16_parse_status_response(packet, words, 0x55667788UL,
                                      &snapshot) == M0_FMC_ERR_VALIDITY);
    return 0;
}

static int test_startup_packet_is_not_response(void)
{
    uint16_t packet[FMC16_MAX_PACKET_WORDS];
    const uint16_t reset_payload[8] = {
        FMC16_SCHEMA_V1_0, 1U, 0U, 0U, 0U, 1U, 0U, 0U
    };
    M0_DataSnapshot snapshot;
    size_t words;

    words = make_xo2_packet(packet, 0x7FU, 0x0000U,
                            reset_payload, 8U, 1U, 0U);
    CHECK(fmc16_validate_packet(packet, words) == M0_FMC_OK);
    CHECK(fmc16_parse_status_response(packet, words, 0x55667788UL,
                                      &snapshot) == M0_FMC_ERR_RESPONSE);
    return 0;
}

int main(void)
{
    int line;

    line = test_command_builder();
    if (line != 0)
    {
        fprintf(stderr, "command builder test failed at line %d\n", line);
        return 1;
    }
    line = test_status_response();
    if (line != 0)
    {
        fprintf(stderr, "status response test failed at line %d\n", line);
        return 2;
    }
    line = test_startup_packet_is_not_response();
    if (line != 0)
    {
        fprintf(stderr, "startup packet test failed at line %d\n", line);
        return 3;
    }
    puts("M0 FMC16 protocol tests: ALL PASS");
    return 0;
}

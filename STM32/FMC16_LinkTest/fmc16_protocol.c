#include "fmc16_protocol.h"

uint16_t fmc16_crc16_words(const uint16_t *words, size_t count)
{
    uint16_t crc = 0xFFFFU;
    size_t i;
    int bit;

    for (i = 0U; i < count; ++i) {
        crc ^= words[i];
        for (bit = 0; bit < 16; ++bit) {
            crc = (uint16_t)(((crc & 0x8000U) != 0U)
                                 ? ((uint16_t)(crc << 1) ^ (uint16_t)0x1021U)
                                 : (uint16_t)(crc << 1));
        }
    }
    return crc;
}

uint32_t fmc16_crc32_words(const uint16_t *words, size_t count)
{
    uint32_t crc = 0xFFFFFFFFUL;
    size_t i;
    int bit;

    for (i = 0U; i < count; ++i) {
        for (bit = 15; bit >= 0; --bit) {
            uint32_t feedback = ((crc >> 31) ^
                                 ((uint32_t)words[i] >> (uint32_t)bit)) & 1UL;
            crc = (feedback != 0UL) ? ((crc << 1) ^ 0x04C11DB7UL)
                                    : (crc << 1);
        }
    }
    return crc;
}

uint8_t fmc16_packet_type(const uint16_t *packet)
{
    return (uint8_t)(packet[4] >> 8);
}

uint8_t fmc16_packet_flags(const uint16_t *packet)
{
    return (uint8_t)packet[4];
}

uint32_t fmc16_packet_sequence(const uint16_t *packet)
{
    return ((uint32_t)packet[8] << 16) | packet[9];
}

uint32_t fmc16_packet_transaction(const uint16_t *packet)
{
    return ((uint32_t)packet[10] << 16) | packet[11];
}

int fmc16_capabilities_compatible(uint32_t capabilities)
{
    return (capabilities & FMC16_REQUIRED_CAPABILITIES) ==
           FMC16_REQUIRED_CAPABILITIES;
}

fmc16_result_t fmc16_validate_packet(const uint16_t *packet,
                                     size_t available_words,
                                     size_t *packet_words)
{
    size_t total;
    size_t payload;
    uint32_t expected_crc32;
    uint32_t received_crc32;

    if ((packet == NULL) || (packet_words == NULL)) {
        return FMC16_ERR_ARGUMENT;
    }
    if (available_words < FMC16_MIN_PACKET_WORDS) {
        return FMC16_ERR_LENGTH;
    }
    if ((packet[0] != FMC16_SOF0) || (packet[1] != FMC16_SOF1)) {
        return FMC16_ERR_SOF;
    }
    if (packet[2] != FMC16_VERSION_HEADER) {
        return FMC16_ERR_VERSION;
    }

    total = packet[3];
    payload = packet[7];
    if ((payload > FMC16_MAX_PAYLOAD_WORDS) ||
        (total != FMC16_MIN_PACKET_WORDS + payload) ||
        (total > FMC16_MAX_PACKET_WORDS) ||
        (total > available_words)) {
        return FMC16_ERR_LENGTH;
    }
    if (fmc16_crc16_words(&packet[2], 10U) != packet[12]) {
        return FMC16_ERR_HEADER_CRC;
    }

    expected_crc32 = fmc16_crc32_words(&packet[2], total - 4U);
    received_crc32 = ((uint32_t)packet[total - 2U] << 16) |
                     packet[total - 1U];
    if (expected_crc32 != received_crc32) {
        return FMC16_ERR_PACKET_CRC;
    }

    *packet_words = total;
    return FMC16_OK;
}

fmc16_result_t fmc16_capture_packet(fmc16_read_word_fn read_word,
                                    void *context,
                                    uint16_t *packet,
                                    size_t capacity_words,
                                    uint32_t max_empty_reads,
                                    size_t *packet_words)
{
    uint32_t reads = 0U;
    uint16_t word;
    size_t total;
    size_t i;

    if ((read_word == NULL) || (packet == NULL) || (packet_words == NULL)) {
        return FMC16_ERR_ARGUMENT;
    }
    if (capacity_words < FMC16_MIN_PACKET_WORDS) {
        return FMC16_ERR_CAPACITY;
    }

    while (reads < max_empty_reads) {
        word = read_word(context);
        ++reads;
        if (word != FMC16_SOF0) {
            continue;
        }
        packet[0] = word;

        word = read_word(context);
        ++reads;
        if (word == FMC16_SOF0) {
            packet[0] = word;
            word = read_word(context);
            ++reads;
        }
        if (word != FMC16_SOF1) {
            continue;
        }
        packet[1] = word;
        packet[2] = read_word(context);
        packet[3] = read_word(context);
        reads += 2U;

        total = packet[3];
        if ((total < FMC16_MIN_PACKET_WORDS) ||
            (total > FMC16_MAX_PACKET_WORDS) ||
            (total > capacity_words)) {
            return FMC16_ERR_LENGTH;
        }
        for (i = 4U; i < total; ++i) {
            packet[i] = read_word(context);
        }
        return fmc16_validate_packet(packet, total, packet_words);
    }
    return FMC16_ERR_TIMEOUT;
}

fmc16_result_t fmc16_write_packet(fmc16_write_word_fn write_word,
                                  void *context,
                                  const uint16_t *packet,
                                  size_t packet_words)
{
    size_t validated_words;
    size_t i;
    fmc16_result_t result;

    if ((write_word == NULL) || (packet == NULL)) {
        return FMC16_ERR_ARGUMENT;
    }
    result = fmc16_validate_packet(packet, packet_words, &validated_words);
    if (result != FMC16_OK) {
        return result;
    }
    for (i = 0U; i < validated_words; ++i) {
        write_word(context, packet[i]);
    }
    return FMC16_OK;
}

fmc16_result_t fmc16_build_get_capabilities(uint16_t *packet,
                                            size_t capacity_words,
                                            uint32_t sequence,
                                            uint32_t transaction_id,
                                            uint8_t flags,
                                            size_t *packet_words)
{
    uint32_t crc32;

    if ((packet == NULL) || (packet_words == NULL)) {
        return FMC16_ERR_ARGUMENT;
    }
    if (capacity_words < 19U) {
        return FMC16_ERR_CAPACITY;
    }

    packet[0] = FMC16_SOF0;
    packet[1] = FMC16_SOF1;
    packet[2] = FMC16_VERSION_HEADER;
    packet[3] = 19U;
    packet[4] = (uint16_t)(((uint16_t)FMC16_TYPE_COMMAND << 8) | flags);
    packet[5] = (uint16_t)(((uint16_t)FMC16_NODE_STM32 << 8) | FMC16_NODE_XO2);
    packet[6] = FMC16_CHANNEL_COMMAND;
    packet[7] = 4U;
    packet[8] = (uint16_t)(sequence >> 16);
    packet[9] = (uint16_t)sequence;
    packet[10] = (uint16_t)(transaction_id >> 16);
    packet[11] = (uint16_t)transaction_id;
    packet[12] = fmc16_crc16_words(&packet[2], 10U);
    packet[13] = FMC16_OPCODE_GET_CAPABILITIES;
    packet[14] = 0x0100U;
    packet[15] = 0U;
    packet[16] = 0U;
    crc32 = fmc16_crc32_words(&packet[2], 15U);
    packet[17] = (uint16_t)(crc32 >> 16);
    packet[18] = (uint16_t)crc32;
    *packet_words = 19U;
    return FMC16_OK;
}

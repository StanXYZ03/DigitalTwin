#ifndef FMC16_PROTOCOL_H
#define FMC16_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define FMC16_SOF0                  0xA55AU
#define FMC16_SOF1                  0x5AA5U
#define FMC16_VERSION_HEADER        0x100DU
#define FMC16_HEADER_WORDS          13U
#define FMC16_MIN_PACKET_WORDS      15U
#define FMC16_MAX_PAYLOAD_WORDS     256U
#define FMC16_MAX_PACKET_WORDS      271U
#define FMC16_EMPTY_WORD            0xFFFFU

#define FMC16_NODE_STM32            0x00U
#define FMC16_NODE_XO2              0x01U
#define FMC16_CHANNEL_LINK          0x0000U
#define FMC16_CHANNEL_STATUS        0x0001U
#define FMC16_CHANNEL_COMMAND       0x0002U
#define FMC16_CHANNEL_EVENT         0x0003U

#define FMC16_TYPE_LINK_HELLO       0x01U
#define FMC16_TYPE_ACK              0x03U
#define FMC16_TYPE_NACK             0x04U
#define FMC16_TYPE_DEVICE_INFO      0x05U
#define FMC16_TYPE_COMMAND          0x30U
#define FMC16_TYPE_RESPONSE         0x31U
#define FMC16_TYPE_RESET_NOTICE     0x7FU

#define FMC16_FLAG_ACK_REQUIRED     0x01U
#define FMC16_FLAG_RETRY            0x80U

#define FMC16_OPCODE_GET_CAPABILITIES 0x0001U
#define FMC16_STATUS_OK             0x0000U
#define FMC16_STATUS_ACCEPTED       0x0001U
#define FMC16_STATUS_BAD_OPCODE     0x0002U

#define FMC16_CAP_ASYNC_READ        0x00000001UL
#define FMC16_CAP_ASYNC_WRITE       0x00000002UL
#define FMC16_REQUIRED_CAPABILITIES \
    (FMC16_CAP_ASYNC_READ | FMC16_CAP_ASYNC_WRITE)

typedef uint16_t (*fmc16_read_word_fn)(void *context);
typedef void (*fmc16_write_word_fn)(void *context, uint16_t word);

typedef enum {
    FMC16_OK = 0,
    FMC16_ERR_ARGUMENT = -1,
    FMC16_ERR_CAPACITY = -2,
    FMC16_ERR_TIMEOUT = -3,
    FMC16_ERR_SOF = -4,
    FMC16_ERR_LENGTH = -5,
    FMC16_ERR_VERSION = -6,
    FMC16_ERR_HEADER_CRC = -7,
    FMC16_ERR_PACKET_CRC = -8,
    FMC16_ERR_FIELDS = -9
} fmc16_result_t;

uint16_t fmc16_crc16_words(const uint16_t *words, size_t count);
uint32_t fmc16_crc32_words(const uint16_t *words, size_t count);

fmc16_result_t fmc16_validate_packet(const uint16_t *packet,
                                     size_t available_words,
                                     size_t *packet_words);

fmc16_result_t fmc16_capture_packet(fmc16_read_word_fn read_word,
                                    void *context,
                                    uint16_t *packet,
                                    size_t capacity_words,
                                    uint32_t max_empty_reads,
                                    size_t *packet_words);

fmc16_result_t fmc16_write_packet(fmc16_write_word_fn write_word,
                                  void *context,
                                  const uint16_t *packet,
                                  size_t packet_words);

fmc16_result_t fmc16_build_get_capabilities(uint16_t *packet,
                                            size_t capacity_words,
                                            uint32_t sequence,
                                            uint32_t transaction_id,
                                            uint8_t flags,
                                            size_t *packet_words);

uint8_t fmc16_packet_type(const uint16_t *packet);
uint8_t fmc16_packet_flags(const uint16_t *packet);
uint32_t fmc16_packet_sequence(const uint16_t *packet);
uint32_t fmc16_packet_transaction(const uint16_t *packet);
int fmc16_capabilities_compatible(uint32_t capabilities);

#endif

#include "m0_data_source.h"

#include <stddef.h>

#ifndef M0_FMC_PROTOCOL_TEST
#include "stm32h7xx_hal.h"
#include "fpga_autoconfig.h"
#endif

#define FMC16_BASE_ADDRESS             0x60000000UL
#define FMC16_SOF0                     0xA55AU
#define FMC16_SOF1                     0x5AA5U
#define FMC16_VERSION_HEADER           0x100DU
#define FMC16_TYPE_COMMAND             0x30U
#define FMC16_TYPE_RESPONSE            0x31U
#define FMC16_CHANNEL_COMMAND          0x0002U
#define FMC16_OPCODE_GET_STATUS        0x0003U
#define FMC16_SCHEMA_V1_0              0x0100U
#define FMC16_STATUS_OK                0x0000U
#define FMC16_COMMAND_WORDS            19U
#define FMC16_RESPONSE_WORDS           29U
#define FMC16_MIN_PACKET_WORDS         15U
#define FMC16_MAX_PACKET_WORDS         64U
#define FMC16_MAX_EMPTY_READS          4096U
#define FMC16_MAX_PACKETS_PER_QUERY    8U
#define FMC16_BRIDGE_SETTLE_US          5U
#define FMC16_VALID_PO                 0x0002U
#define FMC16_VALID_PIO                0x0004U
#define FMC16_VALID_MINIMUM            (FMC16_VALID_PO | FMC16_VALID_PIO)
#define FMC16_VALID_KNOWN_MASK         0x0307U
#define M0_CLOCK_SELECT                23U
#define M0_CLOCK_HZ                    12500000UL

volatile M0_FmcDebug m0_fmc_dbg;

/* One-shot hardware trace retained in RAM for ST-Link inspection.  It records
 * the first transaction only and does not add any extra FMC bus cycles. */
volatile uint16_t m0_fmc_rx_trace[256];
volatile uint16_t m0_fmc_tx_trace[32];
volatile uint32_t m0_fmc_rx_trace_count;
volatile uint32_t m0_fmc_tx_trace_count;
volatile uint32_t m0_po_history[64];
volatile uint32_t m0_timestamp_history[64];
volatile uint16_t m0_pio_history[64];
volatile uint32_t m0_value_history_count;

/* Kept outside the hardware-only section so the host protocol parser test
 * can verify the decoded requested PI fields as well. */
static uint16_t last_pi_applied;

#ifndef M0_FMC_PROTOCOL_TEST
static uint32_t next_command_sequence;
static uint32_t next_transaction_id;
static uint8_t pi_driver_ready;
static uint16_t virtual_pi_toggle;
static uint16_t virtual_key_state;
static uint16_t virtual_key_count;
static uint32_t last_control_id;

static uint8_t m0_sequence_is_newer(uint32_t candidate, uint32_t current)
{
    uint32_t delta;

    delta = candidate - current;
    return ((delta != 0U) && (delta < 0x80000000UL)) ? 1U : 0U;
}

/* Preserve the established exp2-01 wiring (F2..F9 -> PI8..PI15).
 * F1 naturally occupies PI7; F10 wraps to the remaining adjacent free input
 * PI6 because the experiment connector ends at PI15. */
static uint16_t m0_virtual_key_pi_bit(uint8_t f_number)
{
    uint32_t pi_index;

    if ((f_number >= 1U) && (f_number <= 9U))
    {
        pi_index = 6U + (uint32_t)f_number;
    }
    else if (f_number == 10U)
    {
        pi_index = 6U;
    }
    else
    {
        return 0U;
    }
    return (uint16_t)(1UL << pi_index);
}

static void fmc16_delay_cycles(uint32_t cycles)
{
    uint32_t start;

    start = DWT->CYCCNT;
    while ((uint32_t)(DWT->CYCCNT - start) < cycles)
    {
        __NOP();
    }
}

static void fmc16_bridge_enable(void)
{
    /* PH8/Cloud_EN is active low.  Keep the bridge connected only for the
     * command/response transaction so normal SDRAM/LTDC traffic remains
     * isolated from the experiment connector. */
    HAL_GPIO_WritePin(GPIOH, GPIO_PIN_8, GPIO_PIN_RESET);
    __DSB();
    fmc16_delay_cycles(m0_fmc_dbg.bridge_settle_cycles);
    m0_fmc_dbg.bridge_enable_count++;
    m0_fmc_dbg.bridge_gpioh_odr = GPIOH->ODR;
}

static void fmc16_bridge_disable(void)
{
    __DSB();
    HAL_GPIO_WritePin(GPIOH, GPIO_PIN_8, GPIO_PIN_SET);
    __DSB();
    m0_fmc_dbg.bridge_gpioh_odr = GPIOH->ODR;
}

static uint16_t fmc16_read_word(void)
{
    uint16_t word;

    word = *(volatile uint16_t *)FMC16_BASE_ADDRESS;
    if (m0_fmc_rx_trace_count < 256U)
    {
        m0_fmc_rx_trace[m0_fmc_rx_trace_count++] = word;
    }
    m0_fmc_dbg.last_raw_word = word;
    m0_fmc_dbg.rx_word_count++;
    return word;
}

static void fmc16_write_word(uint16_t word)
{
    *(volatile uint16_t *)FMC16_BASE_ADDRESS = word;
    if (m0_fmc_tx_trace_count < 32U)
    {
        m0_fmc_tx_trace[m0_fmc_tx_trace_count++] = word;
    }
    m0_fmc_dbg.tx_word_count++;
}
#endif

static uint16_t fmc16_crc16_words(const uint16_t *words, size_t count)
{
    uint16_t crc;
    size_t i;
    int bit;

    crc = 0xFFFFU;
    for (i = 0U; i < count; ++i)
    {
        crc ^= words[i];
        for (bit = 0; bit < 16; ++bit)
        {
            crc = (uint16_t)(((crc & 0x8000U) != 0U)
                                 ? ((uint16_t)(crc << 1) ^ 0x1021U)
                                 : (uint16_t)(crc << 1));
        }
    }
    return crc;
}

static uint32_t fmc16_crc32_words(const uint16_t *words, size_t count)
{
    uint32_t crc;
    size_t i;
    int bit;

    crc = 0xFFFFFFFFUL;
    for (i = 0U; i < count; ++i)
    {
        for (bit = 15; bit >= 0; --bit)
        {
            uint32_t feedback;

            feedback = ((crc >> 31) ^
                        ((uint32_t)words[i] >> (uint32_t)bit)) & 1UL;
            crc = (feedback != 0UL) ? ((crc << 1) ^ 0x04C11DB7UL)
                                    : (crc << 1);
        }
    }
    return crc;
}

static void fmc16_build_status_command(uint16_t *packet,
                                       uint32_t sequence,
                                       uint32_t transaction)
{
    uint32_t crc32;

    packet[0] = FMC16_SOF0;
    packet[1] = FMC16_SOF1;
    packet[2] = FMC16_VERSION_HEADER;
    packet[3] = FMC16_COMMAND_WORDS;
    packet[4] = (uint16_t)(FMC16_TYPE_COMMAND << 8); /* no ACK_REQUIRED */
    packet[5] = 0x0001U; /* STM32 source, XO2 destination */
    packet[6] = FMC16_CHANNEL_COMMAND;
    packet[7] = 4U;
    packet[8] = (uint16_t)(sequence >> 16);
    packet[9] = (uint16_t)sequence;
    packet[10] = (uint16_t)(transaction >> 16);
    packet[11] = (uint16_t)transaction;
    packet[12] = fmc16_crc16_words(&packet[2], 10U);
    packet[13] = FMC16_OPCODE_GET_STATUS;
    packet[14] = FMC16_SCHEMA_V1_0;
    packet[15] = 0U;
    packet[16] = 0U;
    crc32 = fmc16_crc32_words(&packet[2], 15U);
    packet[17] = (uint16_t)(crc32 >> 16);
    packet[18] = (uint16_t)crc32;
}

static M0_FmcResult fmc16_validate_packet(const uint16_t *packet, size_t words)
{
    uint32_t received_crc;
    uint32_t expected_crc;

    if ((packet == NULL) || (words < FMC16_MIN_PACKET_WORDS) ||
        (packet[3] != words) ||
        (words != (size_t)(FMC16_MIN_PACKET_WORDS + packet[7])))
    {
        return M0_FMC_ERR_LENGTH;
    }
    if ((packet[0] != FMC16_SOF0) || (packet[1] != FMC16_SOF1))
    {
        return M0_FMC_ERR_RESPONSE;
    }
    if (packet[2] != FMC16_VERSION_HEADER)
    {
        return M0_FMC_ERR_VERSION;
    }
    if (fmc16_crc16_words(&packet[2], 10U) != packet[12])
    {
        return M0_FMC_ERR_HEADER_CRC;
    }
    expected_crc = fmc16_crc32_words(&packet[2], words - 4U);
    received_crc = ((uint32_t)packet[words - 2U] << 16) |
                   packet[words - 1U];
    if (expected_crc != received_crc)
    {
        return M0_FMC_ERR_PACKET_CRC;
    }
    return M0_FMC_OK;
}

#ifndef M0_FMC_PROTOCOL_TEST
static M0_FmcResult fmc16_capture_packet(uint16_t *packet,
                                         size_t *packet_words)
{
    uint32_t empty_reads;
    uint16_t word;
    size_t total;
    size_t i;

    empty_reads = 0U;
    *packet_words = 0U;
    while (empty_reads < FMC16_MAX_EMPTY_READS)
    {
        word = fmc16_read_word();
        if (word != FMC16_SOF0)
        {
            empty_reads++;
            continue;
        }
        packet[0] = word;

        word = fmc16_read_word();
        if (word == FMC16_SOF0)
        {
            packet[0] = word;
            word = fmc16_read_word();
        }
        if (word != FMC16_SOF1)
        {
            empty_reads++;
            continue;
        }

        packet[1] = word;
        packet[2] = fmc16_read_word();
        packet[3] = fmc16_read_word();
        total = packet[3];
        if ((total < FMC16_MIN_PACKET_WORDS) ||
            (total > FMC16_MAX_PACKET_WORDS))
        {
            return M0_FMC_ERR_LENGTH;
        }
        for (i = 4U; i < total; ++i)
        {
            packet[i] = fmc16_read_word();
        }
        *packet_words = total;
        return fmc16_validate_packet(packet, total);
    }

    m0_fmc_dbg.timeout_count++;
    return M0_FMC_ERR_TIMEOUT;
}
#endif

static M0_FmcResult fmc16_parse_status_response(const uint16_t *packet,
                                                size_t words,
                                                uint32_t transaction,
                                                M0_DataSnapshot *snapshot)
{
    uint16_t validity;
    uint32_t packet_transaction;
    M0_FmcResult result;

    if ((packet == NULL) || (snapshot == NULL))
    {
        return M0_FMC_ERR_ARGUMENT;
    }
    result = fmc16_validate_packet(packet, words);
    if (result != M0_FMC_OK)
    {
        return result;
    }
    if ((words != FMC16_RESPONSE_WORDS) ||
        ((uint8_t)(packet[4] >> 8) != FMC16_TYPE_RESPONSE) ||
        (packet[5] != 0x0100U) ||
        (packet[6] != FMC16_CHANNEL_COMMAND) ||
        (packet[7] != 14U))
    {
        return M0_FMC_ERR_RESPONSE;
    }

    packet_transaction = ((uint32_t)packet[10] << 16) | packet[11];
    if ((packet_transaction != transaction) ||
        (packet[13] != FMC16_OPCODE_GET_STATUS))
    {
        return M0_FMC_ERR_RESPONSE;
    }

    m0_fmc_dbg.response_status = packet[14];
    m0_fmc_dbg.response_detail = packet[15];
    m0_fmc_dbg.schema = packet[17];
    validity = packet[18];
    m0_fmc_dbg.validity = validity;

    if ((packet[14] != FMC16_STATUS_OK) ||
        (packet[16] != 10U) ||
        (packet[17] != FMC16_SCHEMA_V1_0))
    {
        return M0_FMC_ERR_RESPONSE;
    }
    if (((validity & FMC16_VALID_MINIMUM) != FMC16_VALID_MINIMUM) ||
        ((validity & (uint16_t)~FMC16_VALID_KNOWN_MASK) != 0U))
    {
        return M0_FMC_ERR_VALIDITY;
    }

    snapshot->sequence = ((uint32_t)packet[8] << 16) | packet[9];
    snapshot->timestamp_ms = ((uint32_t)packet[25] << 16) | packet[26];
    snapshot->po = ((uint32_t)packet[22] << 16) | packet[23];
    snapshot->pio = packet[24];
    snapshot->pi_requested = packet[19];
    snapshot->key_state = packet[20];
    snapshot->key_event_count = packet[21];
    snapshot->pi_applied = last_pi_applied;
    snapshot->mode = 0U;
    snapshot->clk_sel = M0_CLOCK_SELECT;
    snapshot->clock_hz = M0_CLOCK_HZ;
    return M0_FMC_OK;
}

#ifndef M0_FMC_PROTOCOL_TEST
void M0_DataSource_Init(void)
{
    volatile uint8_t *debug_bytes;
    size_t i;

    debug_bytes = (volatile uint8_t *)&m0_fmc_dbg;
    for (i = 0U; i < sizeof(m0_fmc_dbg); ++i)
    {
        debug_bytes[i] = 0U;
    }
    next_command_sequence = 1U;
    next_transaction_id = 1U;
    last_pi_applied = 0U;
    virtual_pi_toggle = 0U;
    virtual_key_state = 0U;
    virtual_key_count = 0U;
    last_control_id = 0U;
    /* Reassert the safe value after configuration.  The same value was
     * established before PROGRAM_B was released, so this cannot create an
     * input edge in the user experiment. */
    pi_driver_ready = (FPGA_PI_Init() == HAL_OK) ? 1U : 0U;
    if (pi_driver_ready == 0U)
    {
        m0_fmc_dbg.pi_i2c_error_count++;
    }
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    m0_fmc_dbg.initialized = 1U;
    m0_fmc_dbg.fmc_bcr1 = FMC_Bank1_R->BTCR[0];
    m0_fmc_dbg.fmc_btr1 = FMC_Bank1_R->BTCR[1];
    m0_fmc_dbg.fmc_bwtr1 = FMC_Bank1E_R->BWTR[0];
    m0_fmc_dbg.bridge_settle_cycles =
        (SystemCoreClock / 1000000UL) * FMC16_BRIDGE_SETTLE_US;
}

uint8_t M0_DataSource_Read(M0_DataSnapshot *snapshot)
{
    uint16_t tx_packet[FMC16_COMMAND_WORDS];
    uint16_t rx_packet[FMC16_MAX_PACKET_WORDS];
    size_t rx_words;
    uint32_t transaction;
    uint32_t packet_index;
    uint32_t i;
    M0_FmcResult result;

    if (snapshot == NULL)
    {
        m0_fmc_dbg.last_result = M0_FMC_ERR_ARGUMENT;
        return 0U;
    }
    if (m0_fmc_dbg.initialized == 0U)
    {
        M0_DataSource_Init();
    }

    m0_fmc_dbg.read_attempts++;
    transaction = next_transaction_id++;
    m0_fmc_dbg.command_sequence = next_command_sequence;
    m0_fmc_dbg.transaction_id = transaction;
    fmc16_build_status_command(tx_packet, next_command_sequence++, transaction);

    fmc16_bridge_enable();
    for (i = 0U; i < FMC16_COMMAND_WORDS; ++i)
    {
        fmc16_write_word(tx_packet[i]);
    }
    __DSB();

    result = M0_FMC_ERR_TIMEOUT;
    for (packet_index = 0U;
         packet_index < FMC16_MAX_PACKETS_PER_QUERY;
         ++packet_index)
    {
        rx_words = 0U;
        result = fmc16_capture_packet(rx_packet, &rx_words);
        m0_fmc_dbg.last_result = result;
        m0_fmc_dbg.last_packet_words = (uint16_t)rx_words;
        if (result != M0_FMC_OK)
        {
            if (result == M0_FMC_ERR_TIMEOUT)
            {
                break;
            }
            if (result == M0_FMC_ERR_HEADER_CRC)
            {
                m0_fmc_dbg.header_crc_error_count++;
            }
            else if (result == M0_FMC_ERR_PACKET_CRC)
            {
                m0_fmc_dbg.packet_crc_error_count++;
            }
            else if (result == M0_FMC_ERR_LENGTH)
            {
                m0_fmc_dbg.length_error_count++;
            }
            m0_fmc_dbg.ignored_packets++;
            continue;
        }

        m0_fmc_dbg.last_packet_type = (uint16_t)(rx_packet[4] >> 8);
        m0_fmc_dbg.last_packet_transaction =
            ((uint32_t)rx_packet[10] << 16) | rx_packet[11];
        result = fmc16_parse_status_response(rx_packet, rx_words,
                                             transaction, snapshot);
        if (result == M0_FMC_OK)
        {
            uint32_t history_index;
            uint16_t target_pi;

            target_pi = (uint16_t)(snapshot->pi_requested ^
                                   virtual_pi_toggle);

            if ((pi_driver_ready == 0U) ||
                (target_pi != last_pi_applied))
            {
                if ((pi_driver_ready == 0U) && (FPGA_PI_Init() == HAL_OK))
                {
                    pi_driver_ready = 1U;
                }
                if ((pi_driver_ready != 0U) &&
                    (FPGA_PI_Write(target_pi) == HAL_OK))
                {
                    last_pi_applied = target_pi;
                }
                else
                {
                    pi_driver_ready = 0U;
                    m0_fmc_dbg.pi_i2c_error_count++;
                }
            }
            snapshot->pi_applied = last_pi_applied;
            snapshot->pi_virtual_toggle = virtual_pi_toggle;
            snapshot->virtual_key_state = virtual_key_state;
            snapshot->virtual_key_count = virtual_key_count;
            snapshot->control_ack = last_control_id;

            m0_fmc_dbg.response_sequence = snapshot->sequence;
            m0_fmc_dbg.lattice_timestamp_ms = snapshot->timestamp_ms;
            m0_fmc_dbg.po = snapshot->po;
            m0_fmc_dbg.pio = snapshot->pio;
            m0_fmc_dbg.pi_requested = snapshot->pi_requested;
            m0_fmc_dbg.pi_applied = snapshot->pi_applied;
            m0_fmc_dbg.key_state = snapshot->key_state;
            m0_fmc_dbg.key_event_count = snapshot->key_event_count;
            m0_fmc_dbg.pi_virtual_toggle = virtual_pi_toggle;
            m0_fmc_dbg.virtual_key_state = virtual_key_state;
            m0_fmc_dbg.virtual_key_count = virtual_key_count;
            m0_fmc_dbg.last_control_id = last_control_id;
            m0_fmc_dbg.read_successes++;
            m0_fmc_dbg.last_result = M0_FMC_OK;
            history_index = m0_value_history_count;
            if (history_index < 64U)
            {
                m0_po_history[history_index] = snapshot->po;
                m0_pio_history[history_index] = snapshot->pio;
                m0_timestamp_history[history_index] = snapshot->timestamp_ms;
                m0_value_history_count = history_index + 1U;
            }
            fmc16_bridge_disable();
            return 1U;
        }
        m0_fmc_dbg.last_result = result;
        m0_fmc_dbg.ignored_packets++;
    }

    m0_fmc_dbg.read_failures++;
    m0_fmc_dbg.last_result = result;
    fmc16_bridge_disable();
    return 0U;
}

int32_t M0_DataSource_VirtualKeySet(uint8_t f_number,
                                   uint8_t pressed,
                                   uint32_t command_id)
{
    uint16_t state_bit;
    uint16_t pi_bit;
    uint16_t target_pi;
    uint8_t was_pressed;

    if ((f_number < 1U) || (f_number > 10U) ||
        (pressed > 1U) ||
        (command_id == 0U))
    {
        m0_fmc_dbg.control_reject_count++;
        return -1;
    }
    if (command_id == last_control_id)
    {
        m0_fmc_dbg.control_duplicate_count++;
        return 1;
    }
    if ((last_control_id != 0U) &&
        (m0_sequence_is_newer(command_id, last_control_id) == 0U))
    {
        m0_fmc_dbg.control_reject_count++;
        return -2;
    }

    state_bit = (uint16_t)(1UL << (uint32_t)(f_number - 1U));
    was_pressed = ((virtual_key_state & state_bit) != 0U) ? 1U : 0U;

    /* A down edge is the web equivalent of one physical press.  All ten
     * panel keys have a unique PI event bit.  Release updates held-state
     * telemetry but deliberately does not create a second experiment event. */
    pi_bit = 0U;
    if ((pressed != 0U) && (was_pressed == 0U))
    {
        pi_bit = m0_virtual_key_pi_bit(f_number);
    }

    if (pi_bit != 0U)
    {
        target_pi = (uint16_t)(last_pi_applied ^ pi_bit);
        if ((pi_driver_ready == 0U) && (FPGA_PI_Init() == HAL_OK))
        {
            pi_driver_ready = 1U;
        }
        if ((pi_driver_ready == 0U) ||
            (FPGA_PI_Write(target_pi) != HAL_OK))
        {
            pi_driver_ready = 0U;
            m0_fmc_dbg.pi_i2c_error_count++;
            m0_fmc_dbg.control_reject_count++;
            return -3;
        }
        last_pi_applied = target_pi;
        virtual_pi_toggle ^= pi_bit;
        virtual_key_count++;
    }

    if (pressed != 0U)
    {
        virtual_key_state |= state_bit;
    }
    else
    {
        virtual_key_state &= (uint16_t)~state_bit;
    }
    last_control_id = command_id;
    m0_fmc_dbg.pi_virtual_toggle = virtual_pi_toggle;
    m0_fmc_dbg.virtual_key_state = virtual_key_state;
    m0_fmc_dbg.virtual_key_count = virtual_key_count;
    m0_fmc_dbg.last_control_id = last_control_id;
    return 0;
}
#endif

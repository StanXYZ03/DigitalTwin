#include "ETHDefaultTask.h"
#include "Bsp_ETH.h"
#include "lcd_status.h"
#include "bsp_lcd_rgb.h"
#include "m0_data_source.h"
#include "gpio.h"
#include "fpga_autoconfig.h"
#include "cmsis_os.h"
#include "lwip.h"
#include "lwip/errno.h"
#include "lwip/inet.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include <stdio.h>
#include <string.h>

extern struct netif gnetif;

#define ETH_STARTUP_DELAY_MS      3000U
#define ETH_CONTROL_POLL_PERIOD_MS 10U
#define ETH_CONTROL_PACKET_BYTES  16U
#define ETH_CONTROL_VERSION       1U
#define ETH_CONTROL_ACTION_DOWN   1U
#define ETH_CONTROL_ACTION_UP     2U

typedef struct
{
    uint32_t started;
    uint32_t network_ready;
    uint32_t socket_create_count;
    uint32_t send_count;
    uint32_t send_error_count;
    uint32_t fmc_read_error_count;
    uint32_t last_payload_len;
    uint32_t last_sequence;
    uint32_t last_po;
    uint16_t last_pio;
    uint32_t control_rx_count;
    uint32_t control_accept_count;
    uint32_t control_duplicate_count;
    uint32_t control_reject_count;
    uint32_t last_control_id;
    uint16_t last_control_crc;
    uint8_t last_control_key;
    uint8_t last_control_action;
    int32_t last_fmc_result;
    int32_t last_socket;
    int32_t last_send;
    int32_t last_errno;
} ETH_TaskDebug;

volatile ETH_TaskDebug eth_task_dbg;
static volatile uint8_t network_stack_ready;

#if (FMC_BRIDGE_ISOLATION_DIAG == 1U)
typedef struct
{
    uint32_t samples;
    uint32_t mismatch_count;
    uint16_t bit_error_mask;
    uint16_t last_expected;
    uint16_t last_actual;
    uint16_t last_xor;
    uint16_t complete;
} FMC_SdramBusDebug;

volatile FMC_SdramBusDebug fmc_sdram_bus_dbg;

static void FMC_ConflictBranchDb2HighZ(void)
{
    GPIO_InitTypeDef gpio = {0};

    /* Single-variable diagnostic: release only PG10/R9/DB2.  PD7 must remain
       FMC_NE1 and drive high; releasing it makes the Lattice chip-select float
       and invalidates the test. */
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    gpio.Pin = GPIO_PIN_10;
    HAL_GPIO_Init(GPIOG, &gpio);
}

static void FMC_SdramBusTest(void)
{
    static const uint16_t base_patterns[] = {
        0x0000U, 0xFFFFU, 0xAAAAU, 0x5555U, 0x3333U, 0xCCCCU,
        0x0F0FU, 0xF0F0U, 0x00FFU, 0xFF00U
    };
    volatile uint16_t *test_word = (volatile uint16_t *)0xC0000000U;
    uint32_t pass;
    uint32_t i;
    uint16_t expected;
    uint16_t actual;
    uint16_t difference;

    memset((void *)&fmc_sdram_bus_dbg, 0, sizeof(fmc_sdram_bus_dbg));
    for (pass = 0U; pass < 64U; ++pass)
    {
        for (i = 0U; i < (sizeof(base_patterns) / sizeof(base_patterns[0])); ++i)
        {
            expected = base_patterns[i];
            *test_word = expected;
            __DSB();
            actual = *test_word;
            difference = (uint16_t)(expected ^ actual);
            fmc_sdram_bus_dbg.samples++;
            if (difference != 0U)
            {
                fmc_sdram_bus_dbg.mismatch_count++;
                fmc_sdram_bus_dbg.bit_error_mask |= difference;
                fmc_sdram_bus_dbg.last_expected = expected;
                fmc_sdram_bus_dbg.last_actual = actual;
                fmc_sdram_bus_dbg.last_xor = difference;
            }
        }
        for (i = 0U; i < 16U; ++i)
        {
            expected = (uint16_t)(1UL << i);
            *test_word = expected;
            __DSB();
            actual = *test_word;
            difference = (uint16_t)(expected ^ actual);
            fmc_sdram_bus_dbg.samples++;
            if (difference != 0U)
            {
                fmc_sdram_bus_dbg.mismatch_count++;
                fmc_sdram_bus_dbg.bit_error_mask |= difference;
                fmc_sdram_bus_dbg.last_expected = expected;
                fmc_sdram_bus_dbg.last_actual = actual;
                fmc_sdram_bus_dbg.last_xor = difference;
            }

            expected = (uint16_t)~(1UL << i);
            *test_word = expected;
            __DSB();
            actual = *test_word;
            difference = (uint16_t)(expected ^ actual);
            fmc_sdram_bus_dbg.samples++;
            if (difference != 0U)
            {
                fmc_sdram_bus_dbg.mismatch_count++;
                fmc_sdram_bus_dbg.bit_error_mask |= difference;
                fmc_sdram_bus_dbg.last_expected = expected;
                fmc_sdram_bus_dbg.last_actual = actual;
                fmc_sdram_bus_dbg.last_xor = difference;
            }
        }
    }
    *test_word = 0U;
    __DSB();
    fmc_sdram_bus_dbg.complete = 1U;
}
#endif

static uint8_t ETH_WaitForNetwork(void)
{
    if (network_stack_ready == 0U)
    {
        eth_task_dbg.network_ready = 0U;
        return 0U;
    }

    FPGA_ConfigPins_Sample();

    if (netif_is_up(&gnetif) &&
        netif_is_link_up(&gnetif) &&
        !ip4_addr_isany_val(*netif_ip4_addr(&gnetif)))
    {
        eth_task_dbg.network_ready = 1U;
        return 1U;
    }

    eth_task_dbg.network_ready = 0U;
    return 0U;
}

static int ETH_CreateUdpSocket(void)
{
    int sock;
    unsigned long nonblocking = 1UL;

    sock = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    eth_task_dbg.socket_create_count++;
    eth_task_dbg.last_socket = sock;
    if (sock < 0)
    {
        eth_task_dbg.last_errno = errno;
        return -1;
    }

    (void)lwip_ioctl(sock, FIONBIO, &nonblocking);
    return sock;
}

static void ETH_FillServerAddress(struct sockaddr_in *server_addr)
{
    uint8_t ip[4];
    uint32_t ip_word;

    Bsp_ETH_GetServerIp(ip);
    ip_word = ((uint32_t)ip[0] << 24) |
              ((uint32_t)ip[1] << 16) |
              ((uint32_t)ip[2] << 8) |
              (uint32_t)ip[3];

    memset(server_addr, 0, sizeof(*server_addr));
    server_addr->sin_family = AF_INET;
    server_addr->sin_port = PP_HTONS(Bsp_ETH_GetServerPort());
    server_addr->sin_addr.s_addr = PP_HTONL(ip_word);
}

static uint16_t ETH_ControlCrc16(const uint8_t *data, uint32_t length)
{
    uint16_t crc;
    uint32_t index;
    uint32_t bit;

    crc = 0xFFFFU;
    for (index = 0U; index < length; ++index)
    {
        crc ^= (uint16_t)((uint16_t)data[index] << 8);
        for (bit = 0U; bit < 8U; ++bit)
        {
            crc = (uint16_t)(((crc & 0x8000U) != 0U)
                                 ? ((uint16_t)(crc << 1) ^ 0x1021U)
                                 : (uint16_t)(crc << 1));
        }
    }
    return crc;
}

static void ETH_PollControl(int sock,
                            const struct sockaddr_in *server_addr)
{
    uint8_t packet[ETH_CONTROL_PACKET_BYTES];
    struct sockaddr_in remote_addr;
    socklen_t remote_len;
    int received;
    uint32_t command_id;
    uint16_t received_crc;
    uint16_t expected_crc;
    int32_t result;
    uint32_t packet_index;

    for (packet_index = 0U; packet_index < 8U; ++packet_index)
    {
        remote_len = (socklen_t)sizeof(remote_addr);
        received = lwip_recvfrom(sock, packet, sizeof(packet), 0,
                                 (struct sockaddr *)&remote_addr,
                                 &remote_len);
        if (received < 0)
        {
            if ((errno != EWOULDBLOCK) && (errno != EAGAIN))
            {
                eth_task_dbg.last_errno = errno;
                eth_task_dbg.control_reject_count++;
            }
            return;
        }

        eth_task_dbg.control_rx_count++;
        if ((received != (int)ETH_CONTROL_PACKET_BYTES) ||
            (remote_addr.sin_addr.s_addr != server_addr->sin_addr.s_addr) ||
            (remote_addr.sin_port != server_addr->sin_port) ||
            (packet[0] != (uint8_t)'M') ||
            (packet[1] != (uint8_t)'0') ||
            (packet[2] != (uint8_t)'K') ||
            (packet[3] != (uint8_t)'C') ||
            (packet[4] != ETH_CONTROL_VERSION) ||
            (packet[5] < 1U) || (packet[5] > 10U) ||
            ((packet[6] != ETH_CONTROL_ACTION_DOWN) &&
             (packet[6] != ETH_CONTROL_ACTION_UP)) ||
            (packet[7] != 0U))
        {
            eth_task_dbg.control_reject_count++;
            continue;
        }

        received_crc = (uint16_t)(((uint16_t)packet[14] << 8) |
                                  packet[15]);
        expected_crc = ETH_ControlCrc16(packet, 14U);
        eth_task_dbg.last_control_crc = received_crc;
        if (received_crc != expected_crc)
        {
            eth_task_dbg.control_reject_count++;
            continue;
        }

        command_id = ((uint32_t)packet[8] << 24) |
                     ((uint32_t)packet[9] << 16) |
                     ((uint32_t)packet[10] << 8) |
                     (uint32_t)packet[11];
        result = M0_DataSource_VirtualKeySet(
            packet[5],
            (packet[6] == ETH_CONTROL_ACTION_DOWN) ? 1U : 0U,
            command_id);
        if (result == 0)
        {
            eth_task_dbg.control_accept_count++;
            eth_task_dbg.last_control_id = command_id;
            eth_task_dbg.last_control_key = packet[5];
            eth_task_dbg.last_control_action = packet[6];
        }
        else if (result == 1)
        {
            eth_task_dbg.control_duplicate_count++;
        }
        else
        {
            eth_task_dbg.control_reject_count++;
        }
    }
}

static void ETH_NetworkInitTask(void const *argument)
{
    (void)argument;
    osDelay(ETH_STARTUP_DELAY_MS);
    MX_LWIP_Init();
    __DMB();
    network_stack_ready = 1U;
    (void)osThreadTerminate(osThreadGetId());
    for (;;)
    {
        osDelay(1000U);
    }
}

static int ETH_FormatM0Payload(const M0_DataSnapshot *snapshot,
                               char *payload,
                               size_t payload_size)
{
    return snprintf(
        payload,
        payload_size,
        "{\"type\":\"telemetry\",\"experiment\":\"exp2-01_hex_counter_32\","
        "\"source\":\"fmc16\",\"sequence\":%lu,\"timestamp_ms\":%lu,"
        "\"mode\":%u,\"clk_sel\":%u,\"clock_hz\":%lu,"
        "\"pi_applied\":%u,\"pi_requested\":%u,"
        "\"pi_virtual_toggle\":%u,\"virtual_key_state\":%u,"
        "\"virtual_key_count\":%u,"
        "\"control_ack\":%lu,"
        "\"key_state\":%u,\"key_event_count\":%u,"
        "\"po\":%lu,\"pio\":%u}",
        (unsigned long)snapshot->sequence,
        (unsigned long)snapshot->timestamp_ms,
        (unsigned int)snapshot->mode,
        (unsigned int)snapshot->clk_sel,
        (unsigned long)snapshot->clock_hz,
        (unsigned int)snapshot->pi_applied,
        (unsigned int)snapshot->pi_requested,
        (unsigned int)snapshot->pi_virtual_toggle,
        (unsigned int)snapshot->virtual_key_state,
        (unsigned int)snapshot->virtual_key_count,
        (unsigned long)snapshot->control_ack,
        (unsigned int)snapshot->key_state,
        (unsigned int)snapshot->key_event_count,
        (unsigned long)snapshot->po,
        (unsigned int)snapshot->pio);
}

void ETHDefaultTask(void const *argument)
{
    int sock = -1;
    int payload_len;
    int sent;
    char payload[448];
    struct sockaddr_in server_addr;
    M0_DataSnapshot snapshot;
    uint8_t have_snapshot = 0U;
    uint32_t snapshot_count = 0U;
    uint32_t next_publish_ms;
    uint32_t now_ms;
    osThreadId network_init_handle;

    (void)argument;
    eth_task_dbg.started = 1U;
#if (FPGA_EXTERNAL_JTAG_PASSIVE == 1U)
    /* Hardware Manager owns JTAG/configuration in this build.  Do not pulse
       PROGRAM_B or enable the mode-selection mux behind its back. */
    FPGA_ExternalJtagRelease();
#else
    /* The PI expander was initialized before the scheduler started while
       PROGRAM_B was held low.  Give the bridge rails time to settle, then
       perform one deterministic configuration release. */
    osDelay(1000U);
    (void)FPGA_AutoConfig_Run();

    /* Once DONE is stable, disconnect all U4-U7 bridge muxes. */
    HAL_GPIO_WritePin(GPIOH, GPIO_PIN_7 | GPIO_PIN_8, GPIO_PIN_SET);
#endif
    /* Normal runtime is passive on every branch shared with Xilinx JTAG.
       This permits direct external-JTAG access with the bridge installed. */
    FPGA_ExternalJtagRelease();
    /* Cloud_EN/PH8 is active low.  Begin with the FMC data path isolated. */
    HAL_GPIO_WritePin(GPIOH, GPIO_PIN_8, GPIO_PIN_SET);
    M0_DataSource_Init();
    network_stack_ready = 0U;
    osThreadDef(NetworkInit, ETH_NetworkInitTask,
                osPriorityBelowNormal, 0, 2048);
    network_init_handle = osThreadCreate(osThread(NetworkInit), NULL);
    if (network_init_handle == NULL)
    {
        eth_task_dbg.last_errno = -1;
    }

#if (FMC_BRIDGE_ISOLATION_DIAG == 1U)
    FMC_ConflictBranchDb2HighZ();
    FMC_SdramBusTest();
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.po = fmc_sdram_bus_dbg.mismatch_count;
    snapshot.pio = fmc_sdram_bus_dbg.bit_error_mask;
#endif

    ETH_FillServerAddress(&server_addr);
    next_publish_ms = HAL_GetTick();

    for (;;)
    {
        FPGA_ConfigPins_Sample();

        if (sock >= 0)
        {
            ETH_PollControl(sock, &server_addr);
        }

#if (FMC_BRIDGE_ISOLATION_DIAG == 1U)
        /* A/B test: no NE1 command, no PH8 pulse and no Lattice data drive.
           Keep reporting link state so LCD/LTDC and Ethernet remain active. */
        if (netif_is_up(&gnetif) && netif_is_link_up(&gnetif) &&
            !ip4_addr_isany_val(*netif_ip4_addr(&gnetif)))
        {
            eth_task_dbg.network_ready = 1U;
            LCD_Status_Update(&snapshot, LCD_STATUS_LINK_UP, 0U,
                              fmc_sdram_bus_dbg.mismatch_count);
        }
        else
        {
            eth_task_dbg.network_ready = 0U;
            LCD_Status_Update(&snapshot, LCD_STATUS_WAIT_LINK, 0U,
                              fmc_sdram_bus_dbg.mismatch_count);
        }
        osDelay(500U);
        continue;
#endif

        /* Control reception runs every 10 ms, independently of the existing
         * 100 ms FMC acquisition/UDP telemetry cadence. */
        now_ms = HAL_GetTick();
        if ((int32_t)(now_ms - next_publish_ms) < 0)
        {
            osDelay(ETH_CONTROL_POLL_PERIOD_MS);
            continue;
        }
        next_publish_ms = now_ms + Bsp_ETH_GetPublishPeriodMs();

        if (M0_DataSource_Read(&snapshot) == 0U)
        {
            eth_task_dbg.fmc_read_error_count++;
            eth_task_dbg.last_fmc_result = m0_fmc_dbg.last_result;
            LCD_Status_Update(have_snapshot ? &snapshot : NULL,
                              eth_task_dbg.network_ready ? LCD_STATUS_LINK_UP
                                                         : LCD_STATUS_WAIT_LINK,
                              eth_task_dbg.send_count,
                              eth_task_dbg.send_error_count +
                                  eth_task_dbg.fmc_read_error_count);
            osDelay(ETH_CONTROL_POLL_PERIOD_MS);
            continue;
        }
        have_snapshot = 1U;
        snapshot_count++;
        eth_task_dbg.last_fmc_result = M0_FMC_OK;

        /* Acquisition is independent of Ethernet.  A missing cable or server
         * must not freeze the physical display or hide the experiment state
         * from an attached debugger. */
        if ((snapshot_count % 5U) == 0U)
        {
            LCD_Status_Update(&snapshot,
                              ETH_WaitForNetwork() ? LCD_STATUS_LINK_UP
                                                   : LCD_STATUS_WAIT_LINK,
                              eth_task_dbg.send_count,
                              eth_task_dbg.send_error_count +
                                  eth_task_dbg.fmc_read_error_count);
        }

        if (!ETH_WaitForNetwork())
        {
            osDelay(ETH_CONTROL_POLL_PERIOD_MS);
            continue;
        }

        if (sock < 0)
        {
            sock = ETH_CreateUdpSocket();
            if (sock < 0)
            {
                osDelay(ETH_CONTROL_POLL_PERIOD_MS);
                continue;
            }
        }

        payload_len = ETH_FormatM0Payload(&snapshot, payload, sizeof(payload));
        if ((payload_len <= 0) || ((size_t)payload_len >= sizeof(payload)))
        {
            eth_task_dbg.send_error_count++;
            osDelay(ETH_CONTROL_POLL_PERIOD_MS);
            continue;
        }

        sent = lwip_sendto(sock,
                           payload,
                           (size_t)payload_len,
                           0,
                           (const struct sockaddr *)&server_addr,
                           sizeof(server_addr));

        eth_task_dbg.last_send = sent;
        eth_task_dbg.last_payload_len = (uint32_t)payload_len;
        eth_task_dbg.last_sequence = snapshot.sequence;
        eth_task_dbg.last_po = snapshot.po;
        eth_task_dbg.last_pio = snapshot.pio;

        if (sent < 0)
        {
            eth_task_dbg.last_errno = errno;
            eth_task_dbg.send_error_count++;
            lwip_close(sock);
            sock = -1;
            LCD_Status_Update(&snapshot,
                              LCD_STATUS_SEND_ERROR,
                              eth_task_dbg.send_count,
                              eth_task_dbg.send_error_count);
        }
        else
        {
            eth_task_dbg.last_errno = 0;
            eth_task_dbg.send_count++;
            /* FMC/UDP keeps its 100 ms cadence, but a human-readable LCD
             * does not need to redraw the live counter on every packet. */
            if ((eth_task_dbg.send_count % 5U) == 0U)
            {
                LCD_Status_Update(&snapshot,
                                  LCD_STATUS_SEND_OK,
                                  eth_task_dbg.send_count,
                                  eth_task_dbg.send_error_count);
            }
        }

        osDelay(ETH_CONTROL_POLL_PERIOD_MS);
    }
}

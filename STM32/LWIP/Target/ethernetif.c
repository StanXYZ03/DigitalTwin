/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : ethernetif.c
  * Description        : This file provides code for the configuration
  *                      of the ethernetif.c MiddleWare.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "lwip/opt.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include "netif/etharp.h"
#include "lwip/ethip6.h"
#include "ethernetif.h"
#include "lan8742.h"
#include <string.h>
#include "cmsis_os.h"
#include "lwip/tcpip.h"

/* Within 'USER CODE' section, code will be kept by default at each generation */
/* USER CODE BEGIN 0 */
#include "Bsp_ETH.h"

/* USER CODE END 0 */

/* Private define ------------------------------------------------------------*/
/* The time to block waiting for input. */
#define TIME_WAITING_FOR_INPUT ( portMAX_DELAY )
/* Time to block waiting for transmissions to finish */
#define ETHIF_TX_TIMEOUT (2000U)
/* USER CODE BEGIN OS_THREAD_STACK_SIZE_WITH_RTOS */
/* Stack size of the interface thread */
#define INTERFACE_THREAD_STACK_SIZE ( 1024 )
/* USER CODE END OS_THREAD_STACK_SIZE_WITH_RTOS */
/* Network interface name */
#define IFNAME0 's'
#define IFNAME1 't'

/* ETH Setting  */
#define ETH_DMA_TRANSMIT_TIMEOUT               ( 20U )
#define ETH_TX_BUFFER_MAX             ((ETH_TX_DESC_CNT) * 2U)
/* ETH_RX_BUFFER_SIZE parameter is defined in lwipopts.h */

/* USER CODE BEGIN 1 */
#define LAN8720_SMR_MODE_SHIFT          (5U)
#define LAN8720_SMR_MODE_ALL_CAPABLE    ((uint32_t)0x00E0U)
#define LAN8720_SMR_MODE_WRITE_ONE      ((uint32_t)0x4000U)
#define LAN8720_ANAR_SELECTOR_IEEE8023  ((uint32_t)0x0001U)
#define LAN8720_ANAR_ALL_CAPABLE        ((uint32_t)(LAN8742_ANAR_100BASE_TX_FD | \
                                                    LAN8742_ANAR_100BASE_TX | \
                                                    LAN8742_ANAR_10BASE_T_FD | \
                                                    LAN8742_ANAR_10BASE_T | \
                                                    LAN8720_ANAR_SELECTOR_IEEE8023))

/*
 * LAN8720A/LAN8742 SCSIR bit 15 is AMDIXCTRL: 0 = Auto-MDIX enabled,
 * 1 = Auto-MDIX disabled. The ST macro name looks positive, so keep a local
 * alias to avoid using it backwards in debug code.
 */
#define LAN8720_SCSIR_AMDIXCTRL_DISABLE LAN8742_SCSIR_AUTO_MDIX_ENABLE

/* USER CODE END 1 */

/* Private variables ---------------------------------------------------------*/
/*
@Note: This interface is implemented to operate in zero-copy mode only:
        - Rx Buffers will be allocated from LwIP stack Rx memory pool,
          then passed to ETH HAL driver.
        - Tx Buffers will be allocated from LwIP stack memory heap,
          then passed to ETH HAL driver.

@Notes:
  1.a. ETH DMA Rx descriptors must be contiguous, the default count is 4,
       to customize it please redefine ETH_RX_DESC_CNT in ETH GUI (Rx Descriptor Length)
       so that updated value will be generated in stm32xxxx_hal_conf.h
  1.b. ETH DMA Tx descriptors must be contiguous, the default count is 4,
       to customize it please redefine ETH_TX_DESC_CNT in ETH GUI (Tx Descriptor Length)
       so that updated value will be generated in stm32xxxx_hal_conf.h

  2.a. Rx Buffers number must be between ETH_RX_DESC_CNT and 2*ETH_RX_DESC_CNT
  2.b. Rx Buffers must have the same size: ETH_RX_BUFFER_SIZE, this value must
       passed to ETH DMA in the init field (heth.Init.RxBuffLen)
  2.c  The RX Ruffers addresses and sizes must be properly defined to be aligned
       to L1-CACHE line size (32 bytes).
*/

/* Data Type Definitions */
typedef enum
{
  RX_ALLOC_OK       = 0x00,
  RX_ALLOC_ERROR    = 0x01
} RxAllocStatusTypeDef;

typedef struct
{
  struct pbuf_custom pbuf_custom;
  uint8_t buff[(ETH_RX_BUFFER_SIZE + 31) & ~31] __ALIGNED(32);
} RxBuff_t;

/* Memory Pool Declaration */
#define ETH_RX_BUFFER_CNT             12U
LWIP_MEMPOOL_DECLARE(RX_POOL, ETH_RX_BUFFER_CNT, sizeof(RxBuff_t), "Zero-copy RX PBUF pool");

/* Variable Definitions */
static uint8_t RxAllocStatus;

#if defined ( __ICCARM__ ) /*!< IAR Compiler */

#pragma location=0x30000000
ETH_DMADescTypeDef  DMARxDscrTab[ETH_RX_DESC_CNT]; /* Ethernet Rx DMA Descriptors */
#pragma location=0x30000080
ETH_DMADescTypeDef  DMATxDscrTab[ETH_TX_DESC_CNT]; /* Ethernet Tx DMA Descriptors */

#elif defined ( __CC_ARM )  /* MDK ARM Compiler */

__attribute__((at(0x30000000))) ETH_DMADescTypeDef  DMARxDscrTab[ETH_RX_DESC_CNT]; /* Ethernet Rx DMA Descriptors */
__attribute__((at(0x30000080))) ETH_DMADescTypeDef  DMATxDscrTab[ETH_TX_DESC_CNT]; /* Ethernet Tx DMA Descriptors */

#elif defined ( __GNUC__ ) /* GNU Compiler */

ETH_DMADescTypeDef DMARxDscrTab[ETH_RX_DESC_CNT] __attribute__((section(".RxDecripSection"))); /* Ethernet Rx DMA Descriptors */
ETH_DMADescTypeDef DMATxDscrTab[ETH_TX_DESC_CNT] __attribute__((section(".TxDecripSection")));   /* Ethernet Tx DMA Descriptors */

#endif

#if defined ( __ICCARM__ ) /*!< IAR Compiler */
#pragma location = 0x30000100
extern u8_t memp_memory_RX_POOL_base[];

#elif defined ( __CC_ARM ) /* MDK ARM Compiler */
__attribute__((section(".Rx_PoolSection"))) extern u8_t memp_memory_RX_POOL_base[];

#elif defined ( __GNUC__ ) /* GNU */
__attribute__((section(".Rx_PoolSection"))) extern u8_t memp_memory_RX_POOL_base[];
#endif

/* USER CODE BEGIN 2 */
typedef struct
{
  int32_t hal_init_status;
  uint32_t hal_error;
  int32_t phy_init_status;
  int32_t phy_link_state;
  uint32_t phy_dev_addr;

  uint32_t scan_read_ok_count;
  uint32_t scan_found_addr;
  uint32_t scan_phyid1;
  uint32_t scan_phyid2;

  uint32_t phy_bcr;
  uint32_t phy_bsr;
  uint32_t phy_id1;
  uint32_t phy_id2;
  uint32_t phy_anar;
  uint32_t phy_anlpar;
  uint32_t phy_aner;
  uint32_t phy_mcsr;
  uint32_t phy_smr;
  uint32_t phy_tpdcr;
  uint32_t phy_tcsr;
  uint32_t phy_tcsr_after_start;
  uint32_t cable_diag_count;
  uint32_t cable_diag_status;
  uint32_t cable_diag_type;
  uint32_t cable_diag_length;
  uint32_t phy_scsir;
  uint32_t phy_isfr;
  uint32_t phy_physcsr;
  uint32_t phy_bsr_latched;

  uint32_t lan8720_phy_addr_ok;
  uint32_t lan8720_oui;
  uint32_t lan8720_model;
  uint32_t lan8720_rev;
  uint32_t lan8720_mode;
  uint32_t lan8720_phy_addr_strap;
  uint32_t lan8720_link_bit;
  uint32_t lan8720_autoneg_ability;
  uint32_t lan8720_autoneg_complete;
  uint32_t lan8720_remote_fault;
  uint32_t lan8720_energy_on;
  uint32_t lan8720_auto_mdix;
  uint32_t lan8720_amdix_disabled;
  uint32_t lan8720_mdix_state;
  uint32_t lan8720_speed_100;
  uint32_t lan8720_full_duplex;
  uint32_t phy_normal_mode;
  uint32_t phy_loopback_on;
  uint32_t phy_power_down;
  uint32_t phy_isolate;
  uint32_t phy_autoneg_en;
  uint32_t phy_restart_autoneg;

  uint32_t netif_up;
  uint32_t netif_link_up;
  uint32_t link_poll_count;
  uint32_t link_down_poll_count;
  uint32_t link_recover_count;
  uint32_t mac_restart_count;
  uint32_t mac_watchdog_count;
  uint32_t mac_watchdog_restart_count;
  uint32_t rx_watchdog_count;
  uint32_t rx_watchdog_restart_count;
  uint32_t link_flap_count;
  uint32_t rx_frame_count;
  uint32_t tx_frame_count;
  uint32_t rx_byte_count;
  uint32_t tx_byte_count;
  uint32_t rx_irq_count;
  uint32_t tx_irq_count;
  uint32_t tx_hal_ok_count;
  uint32_t tx_hal_busy_count;
  uint32_t tx_hal_error_count;
  int32_t last_hal_tx_status;
  uint32_t last_hal_error;
  uint32_t eth_error_irq_count;
  uint32_t eth_dma_error;
  uint32_t eth_mac_error;
  uint32_t tx_dma_csr;
  uint32_t tx_maccr;
  uint32_t tx_mtl_tqdr;
  uint32_t tx_desc_tdes0;
  uint32_t tx_desc_tdes1;
  uint32_t tx_desc_tdes2;
  uint32_t tx_desc_tdes3;
  uint32_t rx_alloc_error_count;
  int32_t last_read_status;
  uint32_t rx_input_stage;
  uint32_t rx_input_wake_count;
  int32_t last_netif_input_result;
  uint32_t last_rx_ethertype;
  uint32_t last_tx_ethertype;
  int32_t last_tx_result;
  uint32_t force_netif_up;
  uint32_t force_up_count;
  int32_t force_link_state;
  int32_t arp_announce_result;
  uint32_t arp_announce_count;

  uint32_t rmii_sample_count;
  uint32_t rmii_crs_dv_high_count;
  uint32_t rmii_crs_dv_flip_count;
  uint32_t rmii_rxd0_high_count;
  uint32_t rmii_rxd0_flip_count;
  uint32_t rmii_rxd1_high_count;
  uint32_t rmii_rxd1_flip_count;
  uint32_t rmii_first_sample;
  uint32_t rmii_last_sample;
  uint32_t rmii_tx_sample_count;
  uint32_t rmii_tx_en_high_count;
  uint32_t rmii_tx_en_flip_count;
  uint32_t rmii_txd0_high_count;
  uint32_t rmii_txd0_flip_count;
  uint32_t rmii_txd1_high_count;
  uint32_t rmii_txd1_flip_count;
  uint32_t rmii_tx_first_sample;
  uint32_t rmii_tx_last_sample;

  uint32_t cmd;
  uint32_t cmd_arg;
  int32_t cmd_result;
  uint32_t cmd_count;

  uint32_t refclk_sample_count;
  uint32_t refclk_flip_count;
  uint32_t refclk_high_count;
  uint32_t refclk_low_count;
  uint32_t refclk_first_level;
  uint32_t refclk_last_level;

  uint32_t loop_cmd_count;
  int32_t loop_tx_status;
  int32_t loop_rx_status;
  uint32_t loop_rx_len;
  uint32_t loop_match;
  uint32_t loop_magic;
  uint32_t loop_maccr;
  uint32_t loop_dma_csr;
  uint32_t loop_mtl_rxq;
  uint32_t loop_mtl_txq;
  uint32_t loop_poll_reads;
  uint32_t loop_poll_hit;
} ETH_DebugInfo_t;

volatile ETH_DebugInfo_t eth_dbg = {
  .hal_init_status = -999,
  .hal_error = 0xFFFFFFFFU,
  .phy_init_status = -999,
  .phy_link_state = -999,
  .phy_dev_addr = 0xFFFFFFFFU,
  .scan_found_addr = 0xFFFFFFFFU,
  .scan_phyid1 = 0xFFFFFFFFU,
  .scan_phyid2 = 0xFFFFFFFFU,
  .phy_bcr = 0xFFFFFFFFU,
  .phy_bsr = 0xFFFFFFFFU,
  .phy_id1 = 0xFFFFFFFFU,
  .phy_id2 = 0xFFFFFFFFU,
  .phy_anar = 0xFFFFFFFFU,
  .phy_anlpar = 0xFFFFFFFFU,
  .phy_aner = 0xFFFFFFFFU,
  .phy_mcsr = 0xFFFFFFFFU,
  .phy_smr = 0xFFFFFFFFU,
  .phy_scsir = 0xFFFFFFFFU,
  .phy_isfr = 0xFFFFFFFFU,
  .phy_physcsr = 0xFFFFFFFFU,
  .cmd_result = -999
};
static struct netif *eth_dbg_netif = NULL;

/* USER CODE END 2 */

osSemaphoreId RxPktSemaphore = NULL;   /* Semaphore to signal incoming packets */
osSemaphoreId TxPktSemaphore = NULL;   /* Semaphore to signal transmit packet complete */

/* Global Ethernet handle */
ETH_HandleTypeDef heth;
ETH_TxPacketConfig TxConfig;

/* Private function prototypes -----------------------------------------------*/
static void ethernetif_input(void const * argument);
int32_t ETH_PHY_IO_Init(void);
int32_t ETH_PHY_IO_DeInit (void);
int32_t ETH_PHY_IO_ReadReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t *pRegVal);
int32_t ETH_PHY_IO_WriteReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t RegVal);
int32_t ETH_PHY_IO_GetTick(void);

lan8742_Object_t LAN8742;
lan8742_IOCtx_t  LAN8742_IOCtx = {ETH_PHY_IO_Init,
                                  ETH_PHY_IO_DeInit,
                                  ETH_PHY_IO_WriteReg,
                                  ETH_PHY_IO_ReadReg,
                                  ETH_PHY_IO_GetTick};

/* USER CODE BEGIN 3 */
static void ETH_DebugScanPhy(void);
static void ETH_DebugReadPhyRegs(void);
static void ETH_DebugDecodeLan8720(void);
static int32_t ETH_PHY_RestoreNormalMode(void);
static int32_t ETH_DebugForceNetifUp(int32_t link_state);
static int32_t ETH_DebugSendArpAnnouncement(void);
static void ETH_DebugSampleRmiiRxPins(uint32_t samples);
static void ETH_DebugWatchRmiiRxPins(uint32_t duration_ms);
static void ETH_DebugWatchRmiiTxPins(uint32_t duration_ms);
static void ETH_DebugCaptureTxState(void);
static int32_t ETH_DebugRunCableDiagnostic(void);
static void ETH_DebugHandleCommand(void);
static void ETH_DebugSampleRefClk(uint32_t samples);
static void ETH_DebugRunLoopbackTest(void);
static int32_t ETH_PHY_GetLinkStateCompat(void);

#ifndef LAN8720_ASSUME_LINK_UP
#define LAN8720_ASSUME_LINK_UP 1U
#endif

#ifndef LAN8720_ASSUME_LINK_STATE
#define LAN8720_ASSUME_LINK_STATE LAN8742_STATUS_100MBITS_FULLDUPLEX
#endif

#ifndef LAN8720_FALLBACK_LINK_STATE
#define LAN8720_FALLBACK_LINK_STATE LAN8742_STATUS_100MBITS_FULLDUPLEX
#endif

#ifndef LAN8720_PHY_RESET_SETTLE_MS
#define LAN8720_PHY_RESET_SETTLE_MS 1500U
#endif

#ifndef LAN8720_MAC_WATCHDOG_RESTART_POLLS
#define LAN8720_MAC_WATCHDOG_RESTART_POLLS 200U
#endif

#ifndef LAN8720_RX_WATCHDOG_RESTART_POLLS
#define LAN8720_RX_WATCHDOG_RESTART_POLLS 200U
#endif

static uint8_t ETH_MACAddr[6] = {0x00U, 0x80U, 0xE1U, 0x00U, 0x00U, 0x00U};

/*
 * CubeMX regenerates calls to LAN8742_GetLinkState(). Keep this local macro in
 * a USER CODE block so regenerated code still uses the LAN8720A-compatible
 * status decoder below.
 */
#define LAN8742_GetLinkState(pObj) ETH_PHY_GetLinkStateCompat()

/* USER CODE END 3 */

/* Private functions ---------------------------------------------------------*/
void pbuf_free_custom(struct pbuf *p);

/**
  * @brief  Ethernet Rx Transfer completed callback
  * @param  handlerEth: ETH handler
  * @retval None
  */
void HAL_ETH_RxCpltCallback(ETH_HandleTypeDef *handlerEth)
{
  eth_dbg.rx_irq_count++;
  osSemaphoreRelease(RxPktSemaphore);
}
/**
  * @brief  Ethernet Tx Transfer completed callback
  * @param  handlerEth: ETH handler
  * @retval None
  */
void HAL_ETH_TxCpltCallback(ETH_HandleTypeDef *handlerEth)
{
  eth_dbg.tx_irq_count++;
  osSemaphoreRelease(TxPktSemaphore);
}
/**
  * @brief  Ethernet DMA transfer error callback
  * @param  handlerEth: ETH handler
  * @retval None
  */
void HAL_ETH_ErrorCallback(ETH_HandleTypeDef *handlerEth)
{
  eth_dbg.eth_error_irq_count++;
  eth_dbg.eth_dma_error = HAL_ETH_GetDMAError(handlerEth);
  eth_dbg.eth_mac_error = HAL_ETH_GetMACError(handlerEth);
  if((HAL_ETH_GetDMAError(handlerEth) & ETH_DMACSR_RBU) == ETH_DMACSR_RBU)
  {
     osSemaphoreRelease(RxPktSemaphore);
  }
}

/* USER CODE BEGIN 4 */
static void ETH_DebugScanPhy(void)
{
  uint32_t addr;
  uint32_t phyid1;
  uint32_t phyid2;

  eth_dbg.scan_read_ok_count = 0U;
  eth_dbg.scan_found_addr = 0xFFFFFFFFU;
  eth_dbg.scan_phyid1 = 0xFFFFFFFFU;
  eth_dbg.scan_phyid2 = 0xFFFFFFFFU;

  for(addr = 0U; addr <= 31U; addr++)
  {
    phyid1 = 0xFFFFFFFFU;
    phyid2 = 0xFFFFFFFFU;

    if((ETH_PHY_IO_ReadReg(addr, LAN8742_PHYI1R, &phyid1) == 0) &&
       (ETH_PHY_IO_ReadReg(addr, LAN8742_PHYI2R, &phyid2) == 0))
    {
      if((phyid1 != 0x0000U) && (phyid1 != 0xFFFFU) &&
         (phyid2 != 0x0000U) && (phyid2 != 0xFFFFU))
      {
        eth_dbg.scan_read_ok_count++;
        if(eth_dbg.scan_found_addr == 0xFFFFFFFFU)
        {
          eth_dbg.scan_found_addr = addr;
          eth_dbg.scan_phyid1 = phyid1;
          eth_dbg.scan_phyid2 = phyid2;
        }
      }
    }
  }
}

static void ETH_DebugReadPhyRegs(void)
{
  uint32_t addr = LAN8742.DevAddr;
  uint32_t value;

  eth_dbg.phy_dev_addr = addr;
  eth_dbg.phy_bcr = 0xFFFFFFFFU;
  eth_dbg.phy_bsr = 0xFFFFFFFFU;
  eth_dbg.phy_id1 = 0xFFFFFFFFU;
  eth_dbg.phy_id2 = 0xFFFFFFFFU;
  eth_dbg.phy_anar = 0xFFFFFFFFU;
  eth_dbg.phy_anlpar = 0xFFFFFFFFU;
  eth_dbg.phy_aner = 0xFFFFFFFFU;
  eth_dbg.phy_mcsr = 0xFFFFFFFFU;
  eth_dbg.phy_smr = 0xFFFFFFFFU;
  eth_dbg.phy_tpdcr = 0xFFFFFFFFU;
  eth_dbg.phy_tcsr = 0xFFFFFFFFU;
  eth_dbg.phy_scsir = 0xFFFFFFFFU;
  eth_dbg.phy_isfr = 0xFFFFFFFFU;
  eth_dbg.phy_physcsr = 0xFFFFFFFFU;
  eth_dbg.phy_bsr_latched = 0xFFFFFFFFU;

  if(addr > 31U)
  {
    return;
  }

  if(ETH_PHY_IO_ReadReg(addr, LAN8742_BCR, &value) == 0) { eth_dbg.phy_bcr = value; }
  if(ETH_PHY_IO_ReadReg(addr, LAN8742_BSR, &value) == 0) { eth_dbg.phy_bsr_latched = value; }
  if(ETH_PHY_IO_ReadReg(addr, LAN8742_BSR, &value) == 0) { eth_dbg.phy_bsr = value; }
  if(ETH_PHY_IO_ReadReg(addr, LAN8742_PHYI1R, &value) == 0) { eth_dbg.phy_id1 = value; }
  if(ETH_PHY_IO_ReadReg(addr, LAN8742_PHYI2R, &value) == 0) { eth_dbg.phy_id2 = value; }
  if(ETH_PHY_IO_ReadReg(addr, LAN8742_ANAR, &value) == 0) { eth_dbg.phy_anar = value; }
  if(ETH_PHY_IO_ReadReg(addr, LAN8742_ANLPAR, &value) == 0) { eth_dbg.phy_anlpar = value; }
  if(ETH_PHY_IO_ReadReg(addr, LAN8742_ANER, &value) == 0) { eth_dbg.phy_aner = value; }
  if(ETH_PHY_IO_ReadReg(addr, LAN8742_MCSR, &value) == 0) { eth_dbg.phy_mcsr = value; }
  if(ETH_PHY_IO_ReadReg(addr, LAN8742_SMR, &value) == 0) { eth_dbg.phy_smr = value; }
  if(ETH_PHY_IO_ReadReg(addr, LAN8742_TPDCR, &value) == 0) { eth_dbg.phy_tpdcr = value; }
  if(ETH_PHY_IO_ReadReg(addr, LAN8742_TCSR, &value) == 0) { eth_dbg.phy_tcsr = value; }
  if(ETH_PHY_IO_ReadReg(addr, LAN8742_SCSIR, &value) == 0) { eth_dbg.phy_scsir = value; }
  if(ETH_PHY_IO_ReadReg(addr, LAN8742_ISFR, &value) == 0) { eth_dbg.phy_isfr = value; }
  if(ETH_PHY_IO_ReadReg(addr, LAN8742_PHYSCSR, &value) == 0) { eth_dbg.phy_physcsr = value; }
  ETH_DebugDecodeLan8720();
}

static void ETH_DebugDecodeLan8720(void)
{
  uint32_t phyid1 = eth_dbg.phy_id1 & 0xFFFFU;
  uint32_t phyid2 = eth_dbg.phy_id2 & 0xFFFFU;
  uint32_t bcr = eth_dbg.phy_bcr & 0xFFFFU;
  uint32_t bsr = eth_dbg.phy_bsr & 0xFFFFU;
  uint32_t mcsr = eth_dbg.phy_mcsr & 0xFFFFU;
  uint32_t smr = eth_dbg.phy_smr & 0xFFFFU;
  uint32_t scsir = eth_dbg.phy_scsir & 0xFFFFU;
  uint32_t physcsr = eth_dbg.phy_physcsr & 0xFFFFU;

  eth_dbg.lan8720_phy_addr_ok = (eth_dbg.phy_dev_addr <= 31U) ? 1U : 0U;
  eth_dbg.lan8720_oui = ((phyid1 << 6) | ((phyid2 >> 10) & 0x3FU)) & 0xFFFFFFU;
  eth_dbg.lan8720_model = (phyid2 >> 4) & 0x3FU;
  eth_dbg.lan8720_rev = phyid2 & 0x0FU;
  eth_dbg.lan8720_mode = (smr & LAN8742_SMR_MODE) >> LAN8720_SMR_MODE_SHIFT;
  eth_dbg.lan8720_phy_addr_strap = smr & LAN8742_SMR_PHY_ADDR;

  eth_dbg.lan8720_link_bit = ((bsr & LAN8742_BSR_LINK_STATUS) != 0U) ? 1U : 0U;
  eth_dbg.lan8720_autoneg_ability = ((bsr & LAN8742_BSR_AUTONEGO_ABILITY) != 0U) ? 1U : 0U;
  eth_dbg.lan8720_autoneg_complete = ((bsr & LAN8742_BSR_AUTONEGO_CPLT) != 0U) ? 1U : 0U;
  eth_dbg.lan8720_remote_fault = ((bsr & LAN8742_BSR_REMOTE_FAULT) != 0U) ? 1U : 0U;
  eth_dbg.lan8720_energy_on = ((mcsr & LAN8742_MCSR_ENERGYON) != 0U) ? 1U : 0U;
  eth_dbg.lan8720_amdix_disabled = ((scsir & LAN8720_SCSIR_AMDIXCTRL_DISABLE) != 0U) ? 1U : 0U;
  eth_dbg.lan8720_auto_mdix = (eth_dbg.lan8720_amdix_disabled == 0U) ? 1U : 0U;
  eth_dbg.lan8720_mdix_state = ((scsir & LAN8742_SCSIR_CHANNEL_SELECT) != 0U) ? 1U : 0U;
  eth_dbg.phy_loopback_on = ((bcr & LAN8742_BCR_LOOPBACK) != 0U) ? 1U : 0U;
  eth_dbg.phy_power_down = ((bcr & LAN8742_BCR_POWER_DOWN) != 0U) ? 1U : 0U;
  eth_dbg.phy_isolate = ((bcr & LAN8742_BCR_ISOLATE) != 0U) ? 1U : 0U;
  eth_dbg.phy_autoneg_en = ((bcr & LAN8742_BCR_AUTONEGO_EN) != 0U) ? 1U : 0U;
  eth_dbg.phy_restart_autoneg = ((bcr & LAN8742_BCR_RESTART_AUTONEGO) != 0U) ? 1U : 0U;
  eth_dbg.phy_normal_mode = ((eth_dbg.phy_loopback_on == 0U) &&
                             (eth_dbg.phy_power_down == 0U) &&
                             (eth_dbg.phy_isolate == 0U) &&
                             (eth_dbg.phy_autoneg_en != 0U) &&
                             (eth_dbg.lan8720_auto_mdix != 0U)) ? 1U : 0U;

  eth_dbg.lan8720_speed_100 = 0U;
  eth_dbg.lan8720_full_duplex = 0U;

  if((physcsr & LAN8742_PHYSCSR_HCDSPEEDMASK) == LAN8742_PHYSCSR_100BTX_FD)
  {
    eth_dbg.lan8720_speed_100 = 1U;
    eth_dbg.lan8720_full_duplex = 1U;
  }
  else if((physcsr & LAN8742_PHYSCSR_HCDSPEEDMASK) == LAN8742_PHYSCSR_100BTX_HD)
  {
    eth_dbg.lan8720_speed_100 = 1U;
  }
  else if((physcsr & LAN8742_PHYSCSR_HCDSPEEDMASK) == LAN8742_PHYSCSR_10BT_FD)
  {
    eth_dbg.lan8720_full_duplex = 1U;
  }
}

static int32_t ETH_PHY_RestoreNormalMode(void)
{
  uint32_t addr = LAN8742.DevAddr;
  uint32_t bcr;
  uint32_t smr;
  uint32_t scsir;
  int32_t result;
  uint32_t timeout;

  if(addr > 31U)
  {
    return LAN8742_STATUS_ADDRESS_ERROR;
  }

  result = ETH_PHY_IO_ReadReg(addr, LAN8742_SMR, &smr);
  if(result != 0)
  {
    return result;
  }

  /*
   * The board currently straps MODE[2:0] as 001. Software-reset configuration
   * can use the writable SMR mode bits, so force "all capable/autoneg enabled"
   * here while preserving the PHY address strap bits.
   */
  smr = LAN8720_SMR_MODE_WRITE_ONE |
        LAN8720_SMR_MODE_ALL_CAPABLE |
        (smr & LAN8742_SMR_PHY_ADDR);
  result = ETH_PHY_IO_WriteReg(addr, LAN8742_SMR, smr);
  if(result != 0)
  {
    return result;
  }

  result = ETH_PHY_IO_WriteReg(addr, LAN8742_BCR, LAN8742_BCR_SOFT_RESET);
  if(result != 0)
  {
    return result;
  }

  timeout = HAL_GetTick();
  do
  {
    if(ETH_PHY_IO_ReadReg(addr, LAN8742_BCR, &bcr) != 0)
    {
      return LAN8742_STATUS_READ_ERROR;
    }
  } while(((bcr & LAN8742_BCR_SOFT_RESET) != 0U) &&
          ((HAL_GetTick() - timeout) < 500U));

  if((bcr & LAN8742_BCR_SOFT_RESET) != 0U)
  {
    return LAN8742_STATUS_RESET_TIMEOUT;
  }

  result = ETH_PHY_IO_ReadReg(addr, LAN8742_SCSIR, &scsir);
  if(result != 0)
  {
    return result;
  }

  /* Normal direct-PC mode: enable Auto-MDIX and let autonegotiation decide speed/duplex. */
  scsir &= ~(uint32_t)LAN8720_SCSIR_AMDIXCTRL_DISABLE;
  scsir &= ~(uint32_t)LAN8742_SCSIR_CHANNEL_SELECT;
  result = ETH_PHY_IO_WriteReg(addr, LAN8742_SCSIR, scsir);
  if(result != 0)
  {
    return result;
  }

  result = ETH_PHY_IO_WriteReg(addr, LAN8742_ANAR, LAN8720_ANAR_ALL_CAPABLE);
  if(result != 0)
  {
    return result;
  }

  return ETH_PHY_IO_WriteReg(addr, LAN8742_BCR,
                             LAN8742_BCR_AUTONEGO_EN |
                             LAN8742_BCR_RESTART_AUTONEGO);
}

static int32_t ETH_DebugForceNetifUp(int32_t link_state)
{
  ETH_MACConfigTypeDef mac_cfg = {0};
  uint32_t speed = ETH_SPEED_100M;
  uint32_t duplex = ETH_FULLDUPLEX_MODE;
  uint32_t bcr = LAN8742_BCR_SPEED_SELECT | LAN8742_BCR_DUPLEX_MODE;

  if(eth_dbg_netif == NULL)
  {
    return LAN8742_STATUS_ERROR;
  }

  switch(link_state)
  {
  case LAN8742_STATUS_100MBITS_FULLDUPLEX:
    speed = ETH_SPEED_100M;
    duplex = ETH_FULLDUPLEX_MODE;
    bcr = LAN8742_BCR_SPEED_SELECT | LAN8742_BCR_DUPLEX_MODE;
    break;

  case LAN8742_STATUS_100MBITS_HALFDUPLEX:
    speed = ETH_SPEED_100M;
    duplex = ETH_HALFDUPLEX_MODE;
    bcr = LAN8742_BCR_SPEED_SELECT;
    break;

  case LAN8742_STATUS_10MBITS_FULLDUPLEX:
    speed = ETH_SPEED_10M;
    duplex = ETH_FULLDUPLEX_MODE;
    bcr = LAN8742_BCR_DUPLEX_MODE;
    break;

  case LAN8742_STATUS_10MBITS_HALFDUPLEX:
    speed = ETH_SPEED_10M;
    duplex = ETH_HALFDUPLEX_MODE;
    bcr = 0U;
    break;

  default:
    return LAN8742_STATUS_ERROR;
  }

  if(LAN8742.DevAddr <= 31U)
  {
    (void)ETH_PHY_IO_WriteReg(LAN8742.DevAddr, LAN8742_BCR, bcr);
  }

  HAL_ETH_GetMACConfig(&heth, &mac_cfg);
  mac_cfg.Speed = speed;
  mac_cfg.DuplexMode = duplex;
  HAL_ETH_SetMACConfig(&heth, &mac_cfg);

  HAL_ETH_Start_IT(&heth);
  netif_set_up(eth_dbg_netif);
  netif_set_link_up(eth_dbg_netif);

  eth_dbg.force_netif_up = 1U;
  eth_dbg.force_up_count++;
  eth_dbg.force_link_state = link_state;
  eth_dbg.netif_up = netif_is_up(eth_dbg_netif) ? 1U : 0U;
  eth_dbg.netif_link_up = netif_is_link_up(eth_dbg_netif) ? 1U : 0U;

  return LAN8742_STATUS_OK;
}

static int32_t ETH_DebugSendArpAnnouncement(void)
{
  err_t err;

  if(eth_dbg_netif == NULL)
  {
    return LAN8742_STATUS_ERROR;
  }

  eth_dbg.arp_announce_count++;
  err = etharp_gratuitous(eth_dbg_netif);
  eth_dbg.arp_announce_result = (int32_t)err;

  return (err == ERR_OK) ? LAN8742_STATUS_OK : LAN8742_STATUS_ERROR;
}

static void ETH_DebugSampleRmiiRxPins(uint32_t samples)
{
  uint32_t i;
  uint32_t sample;
  uint32_t last_sample;

  eth_dbg.rmii_sample_count = samples;
  eth_dbg.rmii_crs_dv_high_count = 0U;
  eth_dbg.rmii_crs_dv_flip_count = 0U;
  eth_dbg.rmii_rxd0_high_count = 0U;
  eth_dbg.rmii_rxd0_flip_count = 0U;
  eth_dbg.rmii_rxd1_high_count = 0U;
  eth_dbg.rmii_rxd1_flip_count = 0U;

  last_sample = 0U;
  if((GPIOA->IDR & GPIO_PIN_7) != 0U) { last_sample |= 0x01U; }
  if((GPIOC->IDR & GPIO_PIN_4) != 0U) { last_sample |= 0x02U; }
  if((GPIOC->IDR & GPIO_PIN_5) != 0U) { last_sample |= 0x04U; }

  eth_dbg.rmii_first_sample = last_sample;
  eth_dbg.rmii_last_sample = last_sample;

  for(i = 0U; i < samples; i++)
  {
    sample = 0U;
    if((GPIOA->IDR & GPIO_PIN_7) != 0U) { sample |= 0x01U; }
    if((GPIOC->IDR & GPIO_PIN_4) != 0U) { sample |= 0x02U; }
    if((GPIOC->IDR & GPIO_PIN_5) != 0U) { sample |= 0x04U; }

    if((sample & 0x01U) != 0U) { eth_dbg.rmii_crs_dv_high_count++; }
    if((sample & 0x02U) != 0U) { eth_dbg.rmii_rxd0_high_count++; }
    if((sample & 0x04U) != 0U) { eth_dbg.rmii_rxd1_high_count++; }

    if(((sample ^ last_sample) & 0x01U) != 0U) { eth_dbg.rmii_crs_dv_flip_count++; }
    if(((sample ^ last_sample) & 0x02U) != 0U) { eth_dbg.rmii_rxd0_flip_count++; }
    if(((sample ^ last_sample) & 0x04U) != 0U) { eth_dbg.rmii_rxd1_flip_count++; }

    last_sample = sample;
  }

  eth_dbg.rmii_last_sample = last_sample;
}

static void ETH_DebugWatchRmiiRxPins(uint32_t duration_ms)
{
  uint32_t start_tick;
  uint32_t sample;
  uint32_t last_sample;
  uint32_t loops = 0U;

  eth_dbg.rmii_sample_count = 0U;
  eth_dbg.rmii_crs_dv_high_count = 0U;
  eth_dbg.rmii_crs_dv_flip_count = 0U;
  eth_dbg.rmii_rxd0_high_count = 0U;
  eth_dbg.rmii_rxd0_flip_count = 0U;
  eth_dbg.rmii_rxd1_high_count = 0U;
  eth_dbg.rmii_rxd1_flip_count = 0U;

  last_sample = 0U;
  if((GPIOA->IDR & GPIO_PIN_7) != 0U) { last_sample |= 0x01U; }
  if((GPIOC->IDR & GPIO_PIN_4) != 0U) { last_sample |= 0x02U; }
  if((GPIOC->IDR & GPIO_PIN_5) != 0U) { last_sample |= 0x04U; }

  eth_dbg.rmii_first_sample = last_sample;
  eth_dbg.rmii_last_sample = last_sample;

  start_tick = HAL_GetTick();
  do
  {
    sample = 0U;
    if((GPIOA->IDR & GPIO_PIN_7) != 0U) { sample |= 0x01U; }
    if((GPIOC->IDR & GPIO_PIN_4) != 0U) { sample |= 0x02U; }
    if((GPIOC->IDR & GPIO_PIN_5) != 0U) { sample |= 0x04U; }

    if((sample & 0x01U) != 0U) { eth_dbg.rmii_crs_dv_high_count++; }
    if((sample & 0x02U) != 0U) { eth_dbg.rmii_rxd0_high_count++; }
    if((sample & 0x04U) != 0U) { eth_dbg.rmii_rxd1_high_count++; }

    if(((sample ^ last_sample) & 0x01U) != 0U) { eth_dbg.rmii_crs_dv_flip_count++; }
    if(((sample ^ last_sample) & 0x02U) != 0U) { eth_dbg.rmii_rxd0_flip_count++; }
    if(((sample ^ last_sample) & 0x04U) != 0U) { eth_dbg.rmii_rxd1_flip_count++; }

    last_sample = sample;
    loops++;
  } while((HAL_GetTick() - start_tick) < duration_ms);

  eth_dbg.rmii_sample_count = loops;
  eth_dbg.rmii_last_sample = last_sample;
}

static void ETH_DebugWatchRmiiTxPins(uint32_t duration_ms)
{
  uint32_t start_tick;
  uint32_t sample;
  uint32_t last_sample;
  uint32_t loops = 0U;

  eth_dbg.rmii_tx_sample_count = 0U;
  eth_dbg.rmii_tx_en_high_count = 0U;
  eth_dbg.rmii_tx_en_flip_count = 0U;
  eth_dbg.rmii_txd0_high_count = 0U;
  eth_dbg.rmii_txd0_flip_count = 0U;
  eth_dbg.rmii_txd1_high_count = 0U;
  eth_dbg.rmii_txd1_flip_count = 0U;

  last_sample = 0U;
  if((GPIOG->IDR & GPIO_PIN_11) != 0U) { last_sample |= 0x01U; }
  if((GPIOG->IDR & GPIO_PIN_13) != 0U) { last_sample |= 0x02U; }
  if((GPIOG->IDR & GPIO_PIN_14) != 0U) { last_sample |= 0x04U; }

  eth_dbg.rmii_tx_first_sample = last_sample;
  eth_dbg.rmii_tx_last_sample = last_sample;

  start_tick = HAL_GetTick();
  do
  {
    sample = 0U;
    if((GPIOG->IDR & GPIO_PIN_11) != 0U) { sample |= 0x01U; }
    if((GPIOG->IDR & GPIO_PIN_13) != 0U) { sample |= 0x02U; }
    if((GPIOG->IDR & GPIO_PIN_14) != 0U) { sample |= 0x04U; }

    if((sample & 0x01U) != 0U) { eth_dbg.rmii_tx_en_high_count++; }
    if((sample & 0x02U) != 0U) { eth_dbg.rmii_txd0_high_count++; }
    if((sample & 0x04U) != 0U) { eth_dbg.rmii_txd1_high_count++; }

    if(((sample ^ last_sample) & 0x01U) != 0U) { eth_dbg.rmii_tx_en_flip_count++; }
    if(((sample ^ last_sample) & 0x02U) != 0U) { eth_dbg.rmii_txd0_flip_count++; }
    if(((sample ^ last_sample) & 0x04U) != 0U) { eth_dbg.rmii_txd1_flip_count++; }

    last_sample = sample;
    loops++;
  } while((HAL_GetTick() - start_tick) < duration_ms);

  eth_dbg.rmii_tx_sample_count = loops;
  eth_dbg.rmii_tx_last_sample = last_sample;
}

static void ETH_DebugCaptureTxState(void)
{
  eth_dbg.last_hal_error = HAL_ETH_GetError(&heth);
  eth_dbg.tx_dma_csr = ETH->DMACSR;
  eth_dbg.tx_maccr = ETH->MACCR;
  eth_dbg.tx_mtl_tqdr = ETH->MTLTQDR;
  eth_dbg.tx_desc_tdes0 = DMATxDscrTab[0].DESC0;
  eth_dbg.tx_desc_tdes1 = DMATxDscrTab[0].DESC1;
  eth_dbg.tx_desc_tdes2 = DMATxDscrTab[0].DESC2;
  eth_dbg.tx_desc_tdes3 = DMATxDscrTab[0].DESC3;
}

static int32_t ETH_DebugRunCableDiagnostic(void)
{
  uint32_t addr = LAN8742.DevAddr;
  uint32_t value = 0U;
  uint32_t i;
  int32_t result;

  if(addr > 31U)
  {
    return LAN8742_STATUS_ADDRESS_ERROR;
  }

  eth_dbg.cable_diag_count++;
  eth_dbg.phy_tpdcr = 0xFFFFFFFFU;
  eth_dbg.phy_tcsr = 0xFFFFFFFFU;
  eth_dbg.phy_tcsr_after_start = 0xFFFFFFFFU;
  eth_dbg.cable_diag_status = 0xFFFFFFFFU;
  eth_dbg.cable_diag_type = 0xFFFFFFFFU;
  eth_dbg.cable_diag_length = 0xFFFFFFFFU;

  result = ETH_PHY_IO_ReadReg(addr, LAN8742_TPDCR, &value);
  if(result != 0)
  {
    return result;
  }
  eth_dbg.phy_tpdcr = value;

  result = ETH_PHY_IO_ReadReg(addr, LAN8742_TCSR, &value);
  if(result != 0)
  {
    return result;
  }
  eth_dbg.phy_tcsr = value;

  result = ETH_PHY_IO_WriteReg(addr, LAN8742_TCSR, value | LAN8742_TCSR_TDR_ENABLE);
  if(result != 0)
  {
    return result;
  }

  for(i = 0U; i < 100U; i++)
  {
    result = ETH_PHY_IO_ReadReg(addr, LAN8742_TCSR, &value);
    if(result != 0)
    {
      return result;
    }

    eth_dbg.phy_tcsr_after_start = value;
    if((value & LAN8742_TCSR_TDR_ENABLE) == 0U)
    {
      break;
    }
    osDelay(1U);
  }

  eth_dbg.cable_diag_status = (value & LAN8742_TCSR_TDR_CH_STATUS) ? 1U : 0U;
  eth_dbg.cable_diag_type = (value & LAN8742_TCSR_TDR_CH_CABLE_TYPE) >> 9U;
  eth_dbg.cable_diag_length = value & LAN8742_TCSR_TDR_CH_LENGTH;

  return LAN8742_STATUS_OK;
}

static void ETH_DebugHandleCommand(void)
{
  uint32_t addr = LAN8742.DevAddr;
  uint32_t cmd = eth_dbg.cmd;
  uint32_t bcr = 0U;
  uint32_t scsir = 0U;
  int32_t result = LAN8742_STATUS_OK;

  if(cmd == 0U)
  {
    return;
  }

  if(addr > 31U)
  {
    eth_dbg.cmd_result = LAN8742_STATUS_ADDRESS_ERROR;
    eth_dbg.cmd = 0U;
    return;
  }

  switch(cmd)
  {
  case 1U:
    result = ETH_PHY_IO_WriteReg(addr, LAN8742_BCR, LAN8742_BCR_SOFT_RESET);
    break;

  case 2U:
    result = ETH_PHY_IO_ReadReg(addr, LAN8742_BCR, &bcr);
    if(result == 0)
    {
      bcr |= (LAN8742_BCR_AUTONEGO_EN | LAN8742_BCR_RESTART_AUTONEGO);
      result = ETH_PHY_IO_WriteReg(addr, LAN8742_BCR, bcr);
    }
    break;

  case 3U:
    result = ETH_PHY_IO_WriteReg(addr, LAN8742_BCR,
                                 LAN8742_BCR_SPEED_SELECT |
                                 LAN8742_BCR_DUPLEX_MODE);
    break;

  case 4U:
    result = ETH_PHY_IO_WriteReg(addr, LAN8742_BCR,
                                 LAN8742_BCR_SPEED_SELECT);
    break;

  case 5U:
    result = ETH_PHY_IO_WriteReg(addr, LAN8742_BCR,
                                 LAN8742_BCR_DUPLEX_MODE);
    break;

  case 6U:
    result = ETH_PHY_IO_WriteReg(addr, LAN8742_BCR, 0U);
    break;

  case 7U:
    result = ETH_PHY_IO_WriteReg(addr, LAN8742_BCR,
                                 LAN8742_BCR_LOOPBACK |
                                 LAN8742_BCR_SPEED_SELECT |
                                 LAN8742_BCR_DUPLEX_MODE);
    break;

  case 8U:
    ETH_DebugScanPhy();
    ETH_DebugReadPhyRegs();
    result = LAN8742_STATUS_OK;
    break;

  case 9U:
    /* LAN8720A reg27[15]=0: enable Auto-MDIX */
    result = ETH_PHY_IO_ReadReg(addr, LAN8742_SCSIR, &scsir);
    if(result == 0)
    {
      scsir &= ~(uint32_t)LAN8720_SCSIR_AMDIXCTRL_DISABLE;
      scsir &= ~(uint32_t)LAN8742_SCSIR_CHANNEL_SELECT;
      result = ETH_PHY_IO_WriteReg(addr, LAN8742_SCSIR, scsir);
    }
    break;

  case 10U:
    /* LAN8720A reg27[15]=1, [13]=0: disable Auto-MDIX, force MDI */
    result = ETH_PHY_IO_ReadReg(addr, LAN8742_SCSIR, &scsir);
    if(result == 0)
    {
      scsir |= LAN8720_SCSIR_AMDIXCTRL_DISABLE;
      scsir &= ~(uint32_t)LAN8742_SCSIR_CHANNEL_SELECT;
      result = ETH_PHY_IO_WriteReg(addr, LAN8742_SCSIR, scsir);
    }
    break;

  case 11U:
    /* LAN8720A reg27[15]=1, [13]=1: disable Auto-MDIX, force MDIX */
    result = ETH_PHY_IO_ReadReg(addr, LAN8742_SCSIR, &scsir);
    if(result == 0)
    {
      scsir |= LAN8720_SCSIR_AMDIXCTRL_DISABLE;
      scsir |= LAN8742_SCSIR_CHANNEL_SELECT;
      result = ETH_PHY_IO_WriteReg(addr, LAN8742_SCSIR, scsir);
    }
    break;

  case 12U:
    ETH_DebugSampleRefClk((eth_dbg.cmd_arg == 0U) ? 100000U : eth_dbg.cmd_arg);
    result = LAN8742_STATUS_OK;
    break;

  case 13U:
    ETH_DebugRunLoopbackTest();
    result = LAN8742_STATUS_OK;
    break;

  case 14U:
    ETH_DebugReadPhyRegs();
    result = LAN8742_STATUS_OK;
    break;

  case 15U:
    eth_dbg.force_netif_up = 0U;
    result = ETH_PHY_RestoreNormalMode();
    break;

  case 16U:
    result = ETH_DebugForceNetifUp(LAN8742_STATUS_100MBITS_FULLDUPLEX);
    break;

  case 17U:
    result = ETH_DebugForceNetifUp(LAN8742_STATUS_100MBITS_HALFDUPLEX);
    break;

  case 18U:
    result = ETH_DebugForceNetifUp(LAN8742_STATUS_10MBITS_FULLDUPLEX);
    break;

  case 19U:
    result = ETH_DebugForceNetifUp(LAN8742_STATUS_10MBITS_HALFDUPLEX);
    break;

  case 20U:
    result = ETH_DebugSendArpAnnouncement();
    break;

  case 21U:
    ETH_DebugSampleRmiiRxPins((eth_dbg.cmd_arg == 0U) ? 200000U : eth_dbg.cmd_arg);
    result = LAN8742_STATUS_OK;
    break;

  case 22U:
    ETH_DebugWatchRmiiRxPins((eth_dbg.cmd_arg == 0U) ? 3000U : eth_dbg.cmd_arg);
    result = LAN8742_STATUS_OK;
    break;

  case 23U:
    ETH_DebugCaptureTxState();
    result = LAN8742_STATUS_OK;
    break;

  case 24U:
    ETH_DebugWatchRmiiTxPins((eth_dbg.cmd_arg == 0U) ? 3000U : eth_dbg.cmd_arg);
    result = LAN8742_STATUS_OK;
    break;

  case 25U:
    result = ETH_DebugRunCableDiagnostic();
    break;

  default:
    result = LAN8742_STATUS_ERROR;
    break;
  }

  eth_dbg.cmd_result = result;
  eth_dbg.cmd_count++;
  eth_dbg.cmd = 0U;
  ETH_DebugReadPhyRegs();
}

static void ETH_DebugSampleRefClk(uint32_t samples)
{
  uint32_t i;
  uint32_t level;
  uint32_t last_level;

  eth_dbg.refclk_sample_count = samples;
  eth_dbg.refclk_flip_count = 0U;
  eth_dbg.refclk_high_count = 0U;
  eth_dbg.refclk_low_count = 0U;

  last_level = ((GPIOA->IDR & GPIO_PIN_1) != 0U) ? 1U : 0U;
  eth_dbg.refclk_first_level = last_level;
  eth_dbg.refclk_last_level = last_level;

  for(i = 0U; i < samples; i++)
  {
    level = ((GPIOA->IDR & GPIO_PIN_1) != 0U) ? 1U : 0U;

    if(level != 0U)
    {
      eth_dbg.refclk_high_count++;
    }
    else
    {
      eth_dbg.refclk_low_count++;
    }

    if(level != last_level)
    {
      eth_dbg.refclk_flip_count++;
      last_level = level;
    }
  }

  eth_dbg.refclk_last_level = last_level;
}

static void ETH_DebugRunLoopbackTest(void)
{
  static uint8_t frame[60];
  uint8_t rx_copy[64] = {0};
  ETH_BufferTypeDef tx_buffer = {0};
  ETH_TxPacketConfig test_cfg = {0};
  ETH_MACConfigTypeDef mac_cfg = {0};
  struct pbuf *p = NULL;
  struct pbuf *rx_p = NULL;
  uint32_t i;
  uint16_t rx_len = 0U;
  const uint32_t magic = 0x5AA55AA5U;
  uint32_t addr = LAN8742.DevAddr;

  eth_dbg.loop_cmd_count++;
  eth_dbg.loop_tx_status = -1;
  eth_dbg.loop_rx_status = -1;
  eth_dbg.loop_rx_len = 0U;
  eth_dbg.loop_match = 0U;
  eth_dbg.loop_magic = magic;
  eth_dbg.loop_maccr = ETH->MACCR;
  eth_dbg.loop_dma_csr = ETH->DMACSR;
  eth_dbg.loop_mtl_rxq = ETH->MTLRQDR;
  eth_dbg.loop_mtl_txq = ETH->MTLTQDR;
  eth_dbg.loop_poll_reads = 0U;
  eth_dbg.loop_poll_hit = 0U;

  if(addr <= 31U)
  {
    (void)ETH_PHY_IO_WriteReg(addr, LAN8742_BCR,
                              LAN8742_BCR_LOOPBACK |
                              LAN8742_BCR_SPEED_SELECT |
                              LAN8742_BCR_DUPLEX_MODE);
  }

  HAL_ETH_GetMACConfig(&heth, &mac_cfg);
  mac_cfg.Speed = ETH_SPEED_100M;
  mac_cfg.DuplexMode = ETH_FULLDUPLEX_MODE;
  HAL_ETH_SetMACConfig(&heth, &mac_cfg);

  for(i = 0U; i < 6U; i++)
  {
    frame[i] = heth.Init.MACAddr[i];
    frame[6U + i] = heth.Init.MACAddr[i];
  }

  frame[12] = 0x88U;
  frame[13] = 0xB5U;

  frame[14] = (uint8_t)(magic >> 24);
  frame[15] = (uint8_t)(magic >> 16);
  frame[16] = (uint8_t)(magic >> 8);
  frame[17] = (uint8_t)(magic);

  for(i = 18U; i < sizeof(frame); i++)
  {
    frame[i] = (uint8_t)i;
  }

  tx_buffer.buffer = frame;
  tx_buffer.len = sizeof(frame);
  tx_buffer.next = NULL;

  test_cfg.Attributes = ETH_TX_PACKETS_FEATURES_CSUM | ETH_TX_PACKETS_FEATURES_CRCPAD;
  test_cfg.ChecksumCtrl = ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC;
  test_cfg.CRCPadCtrl = ETH_CRC_PAD_INSERT;
  test_cfg.Length = sizeof(frame);
  test_cfg.TxBuffer = &tx_buffer;
  test_cfg.pData = NULL;

  (void)HAL_ETH_Start_IT(&heth);
  eth_dbg.loop_tx_status = (int32_t)HAL_ETH_Transmit(&heth, &test_cfg, 100U);
  eth_dbg.loop_maccr = ETH->MACCR;
  eth_dbg.loop_dma_csr = ETH->DMACSR;
  eth_dbg.loop_mtl_rxq = ETH->MTLRQDR;
  eth_dbg.loop_mtl_txq = ETH->MTLTQDR;

  for(i = 0U; i < 200U; i++)
  {
    rx_p = NULL;
    eth_dbg.loop_poll_reads++;
    if(HAL_ETH_ReadData(&heth, (void **)&rx_p) == HAL_OK && rx_p != NULL)
    {
      eth_dbg.loop_poll_hit++;
      eth_dbg.loop_rx_status = 0;
      p = rx_p;
      break;
    }
    osDelay(1U);
  }

  eth_dbg.loop_dma_csr = ETH->DMACSR;
  eth_dbg.loop_mtl_rxq = ETH->MTLRQDR;
  eth_dbg.loop_mtl_txq = ETH->MTLTQDR;

  if(p != NULL)
  {
      rx_len = p->tot_len;
      eth_dbg.loop_rx_len = rx_len;

      if(rx_len >= 18U)
      {
        pbuf_copy_partial(p, rx_copy, (rx_len > sizeof(rx_copy)) ? sizeof(rx_copy) : rx_len, 0U);

        if((rx_copy[12] == 0x88U) &&
           (rx_copy[13] == 0xB5U) &&
           (rx_copy[14] == (uint8_t)(magic >> 24)) &&
           (rx_copy[15] == (uint8_t)(magic >> 16)) &&
           (rx_copy[16] == (uint8_t)(magic >> 8)) &&
           (rx_copy[17] == (uint8_t)(magic)))
        {
          eth_dbg.loop_match = 1U;
        }
      }

      pbuf_free(p);
  }
  else
  {
    eth_dbg.loop_rx_status = -1;
  }

  eth_dbg.loop_dma_csr = ETH->DMACSR;
  eth_dbg.loop_mtl_rxq = ETH->MTLRQDR;
  eth_dbg.loop_mtl_txq = ETH->MTLTQDR;

  /* Leave the PHY usable for cable tests after the internal loopback probe. */
  (void)ETH_PHY_RestoreNormalMode();
}

static int32_t ETH_PHY_GetLinkStateCompat(void)
{
  uint32_t addr = LAN8742.DevAddr;
  uint32_t bcr = 0U;
  uint32_t bsr_latched = 0U;
  uint32_t bsr = 0U;
  uint32_t anar = 0U;
  uint32_t anlpar = 0U;
  uint32_t physcsr = 0U;
  uint32_t common = 0U;

  if(addr > 31U)
  {
    return LAN8742_STATUS_ADDRESS_ERROR;
  }

  if(ETH_PHY_IO_ReadReg(addr, LAN8742_BSR, &bsr_latched) != 0)
  {
    return LAN8742_STATUS_READ_ERROR;
  }
  if(ETH_PHY_IO_ReadReg(addr, LAN8742_BSR, &bsr) != 0)
  {
    return LAN8742_STATUS_READ_ERROR;
  }
  eth_dbg.phy_bsr_latched = bsr_latched;
  eth_dbg.phy_bsr = bsr;
  ETH_DebugDecodeLan8720();

  if((bsr & LAN8742_BSR_LINK_STATUS) == 0U)
  {
#if (LAN8720_ASSUME_LINK_UP != 0U)
    return LAN8720_ASSUME_LINK_STATE;
#else
    return LAN8742_STATUS_LINK_DOWN;
#endif
  }

  if(ETH_PHY_IO_ReadReg(addr, LAN8742_BCR, &bcr) != 0)
  {
    return LAN8742_STATUS_READ_ERROR;
  }

  if((bcr & LAN8742_BCR_AUTONEGO_EN) == 0U)
  {
    if(((bcr & LAN8742_BCR_SPEED_SELECT) != 0U) && ((bcr & LAN8742_BCR_DUPLEX_MODE) != 0U))
    {
      return LAN8742_STATUS_100MBITS_FULLDUPLEX;
    }
    if((bcr & LAN8742_BCR_SPEED_SELECT) != 0U)
    {
      return LAN8742_STATUS_100MBITS_HALFDUPLEX;
    }
    if((bcr & LAN8742_BCR_DUPLEX_MODE) != 0U)
    {
      return LAN8742_STATUS_10MBITS_FULLDUPLEX;
    }
    return LAN8742_STATUS_10MBITS_HALFDUPLEX;
  }

  if((bsr & LAN8742_BSR_AUTONEGO_CPLT) != 0U)
  {
    if(ETH_PHY_IO_ReadReg(addr, LAN8742_PHYSCSR, &physcsr) == 0)
    {
      uint32_t speed_status = physcsr & LAN8742_PHYSCSR_HCDSPEEDMASK;
      eth_dbg.phy_physcsr = physcsr;

      if(((physcsr & 0xFFFFU) != 0xFFFFU) &&
         ((physcsr & 0xFFFFU) != 0x0000U))
      {
        if(speed_status == LAN8742_PHYSCSR_100BTX_FD)
        {
          return LAN8742_STATUS_100MBITS_FULLDUPLEX;
        }
        if(speed_status == LAN8742_PHYSCSR_100BTX_HD)
        {
          return LAN8742_STATUS_100MBITS_HALFDUPLEX;
        }
        if(speed_status == LAN8742_PHYSCSR_10BT_FD)
        {
          return LAN8742_STATUS_10MBITS_FULLDUPLEX;
        }
        if(speed_status == LAN8742_PHYSCSR_10BT_HD)
        {
          return LAN8742_STATUS_10MBITS_HALFDUPLEX;
        }
      }
    }
  }

  /*
   * Some LAN8720A boards report the standard BSR link bit before the generated
   * LAN8742 PHYSCSR/autoneg fields become trustworthy. If the PC already sees a
   * cable, allow the MAC to come up and use ANAR/ANLPAR as the best available
   * speed/duplex hint instead of keeping lwIP down forever.
   */
  if((ETH_PHY_IO_ReadReg(addr, LAN8742_ANAR, &anar) == 0) &&
     (ETH_PHY_IO_ReadReg(addr, LAN8742_ANLPAR, &anlpar) == 0))
  {
    common = anar & anlpar;

    if((common & LAN8742_ANLPAR_100BASE_TX_FD) != 0U)
    {
      return LAN8742_STATUS_100MBITS_FULLDUPLEX;
    }
    if((common & LAN8742_ANLPAR_100BASE_TX) != 0U)
    {
      return LAN8742_STATUS_100MBITS_HALFDUPLEX;
    }
    if((common & LAN8742_ANLPAR_10BASE_T_FD) != 0U)
    {
      return LAN8742_STATUS_10MBITS_FULLDUPLEX;
    }
    if((common & LAN8742_ANLPAR_10BASE_T) != 0U)
    {
      return LAN8742_STATUS_10MBITS_HALFDUPLEX;
    }
  }

  return LAN8720_FALLBACK_LINK_STATE;
}

/* USER CODE END 4 */

/*******************************************************************************
                       LL Driver Interface ( LwIP stack --> ETH)
*******************************************************************************/
/**
 * @brief In this function, the hardware should be initialized.
 * Called from ethernetif_init().
 *
 * @param netif the already initialized lwip network interface structure
 *        for this ethernetif
 */
static void low_level_init(struct netif *netif)
{
  HAL_StatusTypeDef hal_eth_init_status = HAL_OK;
  uint32_t duplex, speed = 0;
  int32_t PHYLinkState = 0;
  ETH_MACConfigTypeDef MACConf = {0};
  /* Start ETH HAL Init */

  heth.Instance = ETH;
  heth.Init.MACAddr = &ETH_MACAddr[0];
  heth.Init.MediaInterface = HAL_ETH_RMII_MODE;
  heth.Init.TxDesc = DMATxDscrTab;
  heth.Init.RxDesc = DMARxDscrTab;
  heth.Init.RxBuffLen = 1536;

  /* USER CODE BEGIN MACADDRESS */
  /*
   * Keep the MAC address in static storage. CubeMX may regenerate a local
   * MACAddr[] above; overriding the pointer here prevents heth.Init.MACAddr
   * from pointing at a dead stack object after low_level_init() returns.
   */
  heth.Init.MACAddr = &ETH_MACAddr[0];

  /* USER CODE END MACADDRESS */

  hal_eth_init_status = HAL_ETH_Init(&heth);

  memset(&TxConfig, 0 , sizeof(ETH_TxPacketConfig));
  TxConfig.Attributes = ETH_TX_PACKETS_FEATURES_CSUM | ETH_TX_PACKETS_FEATURES_CRCPAD;
  TxConfig.ChecksumCtrl = ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC;
  TxConfig.CRCPadCtrl = ETH_CRC_PAD_INSERT;

  /* End ETH HAL Init */

  /* Initialize the RX POOL */
  LWIP_MEMPOOL_INIT(RX_POOL);

#if LWIP_ARP || LWIP_ETHERNET

  /* set MAC hardware address length */
  netif->hwaddr_len = ETH_HWADDR_LEN;

  /* set MAC hardware address */
  netif->hwaddr[0] =  heth.Init.MACAddr[0];
  netif->hwaddr[1] =  heth.Init.MACAddr[1];
  netif->hwaddr[2] =  heth.Init.MACAddr[2];
  netif->hwaddr[3] =  heth.Init.MACAddr[3];
  netif->hwaddr[4] =  heth.Init.MACAddr[4];
  netif->hwaddr[5] =  heth.Init.MACAddr[5];

  /* maximum transfer unit */
  netif->mtu = ETH_MAX_PAYLOAD;

  /* Accept broadcast address and ARP traffic */
  /* don't set NETIF_FLAG_ETHARP if this device is not an ethernet one */
  #if LWIP_ARP
    netif->flags |= NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
  #else
    netif->flags |= NETIF_FLAG_BROADCAST;
  #endif /* LWIP_ARP */

  /* create a binary semaphore used for informing ethernetif of frame reception */
  osSemaphoreDef(RxSem);
  RxPktSemaphore = osSemaphoreCreate(osSemaphore(RxSem), 1);

  /* create a binary semaphore used for informing ethernetif of frame transmission */
  osSemaphoreDef(TxSem);
  TxPktSemaphore = osSemaphoreCreate(osSemaphore(TxSem), 1);

  /* Decrease the semaphore's initial count from 1 to 0 */
  osSemaphoreWait(RxPktSemaphore, 0);
  osSemaphoreWait(TxPktSemaphore, 0);

  /* create the task that handles the ETH_MAC */
/* USER CODE BEGIN OS_THREAD_DEF_CREATE_CMSIS_RTOS_V1 */
  osThreadDef(EthIf, ethernetif_input, osPriorityRealtime, 0, INTERFACE_THREAD_STACK_SIZE);
  osThreadCreate (osThread(EthIf), netif);
/* USER CODE END OS_THREAD_DEF_CREATE_CMSIS_RTOS_V1 */

/* USER CODE BEGIN PHY_PRE_CONFIG */

/* USER CODE END PHY_PRE_CONFIG */
  /* Set PHY IO functions */
  LAN8742_RegisterBusIO(&LAN8742, &LAN8742_IOCtx);

  /* Initialize the LAN8742 ETH PHY */
  eth_dbg.hal_init_status = (int32_t)hal_eth_init_status;
  eth_dbg.phy_init_status = LAN8742_Init(&LAN8742);
  if(eth_dbg.phy_init_status != LAN8742_STATUS_OK)
  {
    netif_set_link_down(netif);
    netif_set_down(netif);
    return;
  }

  /*
   * LAN8720A cold power-up can be marginal if the MAC touches it while the
   * 50 MHz RMII clock and strap/autoneg state are still settling. Do a PHY
   * soft reset here, after MDIO works, then restart normal autoneg mode.
   */
  (void)ETH_PHY_RestoreNormalMode();
  osDelay(LAN8720_PHY_RESET_SETTLE_MS);

  if (hal_eth_init_status == HAL_OK)
  {
    PHYLinkState = ETH_PHY_GetLinkStateCompat();
    eth_dbg.phy_link_state = PHYLinkState;

    /* Get link state */
    if(PHYLinkState <= LAN8742_STATUS_LINK_DOWN)
    {
      netif_set_link_down(netif);
      netif_set_down(netif);
    }
    else
    {
      switch (PHYLinkState)
      {
      case LAN8742_STATUS_100MBITS_FULLDUPLEX:
        duplex = ETH_FULLDUPLEX_MODE;
        speed = ETH_SPEED_100M;
        break;
      case LAN8742_STATUS_100MBITS_HALFDUPLEX:
        duplex = ETH_HALFDUPLEX_MODE;
        speed = ETH_SPEED_100M;
        break;
      case LAN8742_STATUS_10MBITS_FULLDUPLEX:
        duplex = ETH_FULLDUPLEX_MODE;
        speed = ETH_SPEED_10M;
        break;
      case LAN8742_STATUS_10MBITS_HALFDUPLEX:
        duplex = ETH_HALFDUPLEX_MODE;
        speed = ETH_SPEED_10M;
        break;
      default:
        duplex = ETH_FULLDUPLEX_MODE;
        speed = ETH_SPEED_100M;
        break;
      }

    /* Get MAC Config MAC */
    HAL_ETH_GetMACConfig(&heth, &MACConf);
    MACConf.DuplexMode = duplex;
    MACConf.Speed = speed;
    HAL_ETH_SetMACConfig(&heth, &MACConf);

    HAL_ETH_Start_IT(&heth);
    netif_set_up(netif);
    netif_set_link_up(netif);

/* USER CODE BEGIN PHY_POST_CONFIG */

/* USER CODE END PHY_POST_CONFIG */
    }

  }
  else
  {
    Error_Handler();
  }
#endif /* LWIP_ARP || LWIP_ETHERNET */

/* USER CODE BEGIN LOW_LEVEL_INIT */
  eth_dbg.netif_up = netif_is_up(netif) ? 1U : 0U;
  eth_dbg.netif_link_up = netif_is_link_up(netif) ? 1U : 0U;
  ETH_DebugCaptureTxState();

/* USER CODE END LOW_LEVEL_INIT */
}

/**
 * @brief This function should do the actual transmission of the packet. The packet is
 * contained in the pbuf that is passed to the function. This pbuf
 * might be chained.
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @param p the MAC packet to send (e.g. IP packet including MAC addresses and type)
 * @return ERR_OK if the packet could be sent
 *         an err_t value if the packet couldn't be sent
 *
 * @note Returning ERR_MEM here if a DMA queue of your MAC is full can lead to
 *       strange results. You might consider waiting for space in the DMA queue
 *       to become available since the stack doesn't retry to send a packet
 *       dropped because of memory failure (except for the TCP timers).
 */

static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
  uint32_t i = 0U;
  struct pbuf *q = NULL;
  err_t errval = ERR_OK;
  ETH_BufferTypeDef Txbuffer[ETH_TX_DESC_CNT] = {0};

  memset(Txbuffer, 0 , ETH_TX_DESC_CNT*sizeof(ETH_BufferTypeDef));

  for(q = p; q != NULL; q = q->next)
  {
    if(i >= ETH_TX_DESC_CNT)
      return ERR_IF;

    Txbuffer[i].buffer = q->payload;
    Txbuffer[i].len = q->len;

    /* The H743 D-Cache is enabled while ETH DMA reads physical memory.
       Clean every payload fragment before handing it to DMA. */
    {
      uint32_t cache_start = ((uint32_t)q->payload) & ~31U;
      uint32_t cache_end = (((uint32_t)q->payload) + q->len + 31U) & ~31U;
      SCB_CleanDCache_by_Addr((uint32_t *)cache_start,
                             (int32_t)(cache_end - cache_start));
    }

    if(i>0)
    {
      Txbuffer[i-1].next = &Txbuffer[i];
    }

    if(q->next == NULL)
    {
      Txbuffer[i].next = NULL;
    }

    i++;
  }

  TxConfig.Length = p->tot_len;
  TxConfig.TxBuffer = Txbuffer;
  TxConfig.pData = p;

  pbuf_ref(p);

  eth_dbg.tx_frame_count++;
  eth_dbg.tx_byte_count += p->tot_len;
  if(p->len >= 14U)
  {
    uint8_t *frame = (uint8_t *)p->payload;
    eth_dbg.last_tx_ethertype = ((uint32_t)frame[12] << 8) | frame[13];
  }

  do
  {
    if(HAL_ETH_Transmit_IT(&heth, &TxConfig) == HAL_OK)
    {
      eth_dbg.tx_hal_ok_count++;
      eth_dbg.last_hal_tx_status = HAL_OK;
      errval = ERR_OK;
    }
    else
    {
      eth_dbg.last_hal_tx_status = HAL_ERROR;
      eth_dbg.last_hal_error = HAL_ETH_GetError(&heth);

      if(HAL_ETH_GetError(&heth) & HAL_ETH_ERROR_BUSY)
      {
        eth_dbg.tx_hal_busy_count++;
        /* Wait for descriptors to become available */
        osSemaphoreWait(TxPktSemaphore, ETHIF_TX_TIMEOUT);
        HAL_ETH_ReleaseTxPacket(&heth);
        errval = ERR_BUF;
      }
      else
      {
        eth_dbg.tx_hal_error_count++;
        /* Other error */
        pbuf_free(p);
        errval =  ERR_IF;
      }
    }
  }while(errval == ERR_BUF);

  return errval;
}

/**
 * @brief Should allocate a pbuf and transfer the bytes of the incoming
 * packet from the interface into the pbuf.
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @return a pbuf filled with the received packet (including MAC header)
 *         NULL on memory error
   */
static struct pbuf * low_level_input(struct netif *netif)
{
  struct pbuf *p = NULL;

  if(RxAllocStatus == RX_ALLOC_OK)
  {
    eth_dbg.rx_input_stage = 2U;
    eth_dbg.last_read_status = (int32_t)HAL_ETH_ReadData(&heth, (void **)&p);
    if(p != NULL)
    {
      eth_dbg.rx_frame_count++;
      eth_dbg.rx_byte_count += p->tot_len;
      if(p->len >= 14U)
      {
        uint8_t *frame = (uint8_t *)p->payload;
        eth_dbg.last_rx_ethertype = ((uint32_t)frame[12] << 8) | frame[13];
      }
    }
  }

  return p;
}

/**
 * @brief This function should be called when a packet is ready to be read
 * from the interface. It uses the function low_level_input() that
 * should handle the actual reception of bytes from the network
 * interface. Then the type of the received packet is determined and
 * the appropriate input function is called.
 *
 * @param netif the lwip network interface structure for this ethernetif
 */
static void ethernetif_input(void const * argument)
{
  struct pbuf *p = NULL;
  struct netif *netif = (struct netif *) argument;

  for( ;; )
  {
    if (osSemaphoreWait(RxPktSemaphore, TIME_WAITING_FOR_INPUT) == osOK)
    {
      eth_dbg.rx_input_stage = 1U;
      eth_dbg.rx_input_wake_count++;
      do
      {
        p = low_level_input( netif );
        if (p != NULL)
        {
          eth_dbg.rx_input_stage = 3U;
          eth_dbg.last_netif_input_result = (int32_t)netif->input( p, netif);
          eth_dbg.rx_input_stage = 4U;
          if (eth_dbg.last_netif_input_result != ERR_OK )
          {
            pbuf_free(p);
          }
        }
      } while(p!=NULL);
    }
  }
}

#if !LWIP_ARP
/**
 * This function has to be completed by user in case of ARP OFF.
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @return ERR_OK if ...
 */
static err_t low_level_output_arp_off(struct netif *netif, struct pbuf *q, const ip4_addr_t *ipaddr)
{
  err_t errval;
  errval = ERR_OK;

/* USER CODE BEGIN 5 */

/* USER CODE END 5 */

  return errval;

}
#endif /* LWIP_ARP */

/**
 * @brief Should be called at the beginning of the program to set up the
 * network interface. It calls the function low_level_init() to do the
 * actual setup of the hardware.
 *
 * This function should be passed as a parameter to netif_add().
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @return ERR_OK if the loopif is initialized
 *         ERR_MEM if private data couldn't be allocated
 *         any other err_t on error
 */
err_t ethernetif_init(struct netif *netif)
{
  LWIP_ASSERT("netif != NULL", (netif != NULL));

#if LWIP_NETIF_HOSTNAME
  /* Initialize interface hostname */
  netif->hostname = "lwip";
#endif /* LWIP_NETIF_HOSTNAME */

  /*
   * Initialize the snmp variables and counters inside the struct netif.
   * The last argument should be replaced with your link speed, in units
   * of bits per second.
   */
  // MIB2_INIT_NETIF(netif, snmp_ifType_ethernet_csmacd, LINK_SPEED_OF_YOUR_NETIF_IN_BPS);

  netif->name[0] = IFNAME0;
  netif->name[1] = IFNAME1;
  /* We directly use etharp_output() here to save a function call.
   * You can instead declare your own function an call etharp_output()
   * from it if you have to do some checks before sending (e.g. if link
   * is available...) */

#if LWIP_IPV4
#if LWIP_ARP || LWIP_ETHERNET
#if LWIP_ARP
  netif->output = etharp_output;
#else
  /* The user should write its own code in low_level_output_arp_off function */
  netif->output = low_level_output_arp_off;
#endif /* LWIP_ARP */
#endif /* LWIP_ARP || LWIP_ETHERNET */
#endif /* LWIP_IPV4 */

#if LWIP_IPV6
  netif->output_ip6 = ethip6_output;
#endif /* LWIP_IPV6 */

  netif->linkoutput = low_level_output;

  /* initialize the hardware */
  low_level_init(netif);

  return ERR_OK;
}

/**
  * @brief  Custom Rx pbuf free callback
  * @param  pbuf: pbuf to be freed
  * @retval None
  */
void pbuf_free_custom(struct pbuf *p)
{
  struct pbuf_custom* custom_pbuf = (struct pbuf_custom*)p;
  LWIP_MEMPOOL_FREE(RX_POOL, custom_pbuf);

  /* If the Rx Buffer Pool was exhausted, signal the ethernetif_input task to
   * call HAL_ETH_GetRxDataBuffer to rebuild the Rx descriptors. */

  if (RxAllocStatus == RX_ALLOC_ERROR)
  {
    RxAllocStatus = RX_ALLOC_OK;
    osSemaphoreRelease(RxPktSemaphore);
  }
}

/* USER CODE BEGIN 6 */

/**
* @brief  Returns the current time in milliseconds
*         when LWIP_TIMERS == 1 and NO_SYS == 1
* @param  None
* @retval Current Time value
*/
u32_t sys_now(void)
{
  return HAL_GetTick();
}

/* USER CODE END 6 */

/**
  * @brief  Initializes the ETH MSP.
  * @param  ethHandle: ETH handle
  * @retval None
  */

void HAL_ETH_MspInit(ETH_HandleTypeDef* ethHandle)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if(ethHandle->Instance==ETH)
  {
  /* USER CODE BEGIN ETH_MspInit 0 */

  /* USER CODE END ETH_MspInit 0 */
    /* Enable Peripheral clock */
    __HAL_RCC_ETH1MAC_CLK_ENABLE();
    __HAL_RCC_ETH1TX_CLK_ENABLE();
    __HAL_RCC_ETH1RX_CLK_ENABLE();

    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();
    /**ETH GPIO Configuration
    PC1     ------> ETH_MDC
    PA1     ------> ETH_REF_CLK
    PA2     ------> ETH_MDIO
    PA7     ------> ETH_CRS_DV
    PC4     ------> ETH_RXD0
    PC5     ------> ETH_RXD1
    PG11     ------> ETH_TX_EN
    PG13     ------> ETH_TXD0
    PG14     ------> ETH_TXD1
    */
    GPIO_InitStruct.Pin = GPIO_PIN_1|GPIO_PIN_4|GPIO_PIN_5;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF11_ETH;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_7;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF11_ETH;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_11|GPIO_PIN_13|GPIO_PIN_14;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF11_ETH;
    HAL_GPIO_Init(GPIOG, &GPIO_InitStruct);

  /* USER CODE BEGIN ETH_MspInit 1 */
    /*
     * CubeMX may regenerate the ETH GPIO blocks with LOW speed. Keep this
     * user-section override so the 50 MHz RMII pins always use a fast driver.
     */
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF11_ETH;

    GPIO_InitStruct.Pin = GPIO_PIN_1|GPIO_PIN_4|GPIO_PIN_5;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_7;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_11|GPIO_PIN_13|GPIO_PIN_14;
    HAL_GPIO_Init(GPIOG, &GPIO_InitStruct);

    HAL_NVIC_SetPriority(ETH_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(ETH_IRQn);

  /* USER CODE END ETH_MspInit 1 */
  }
}

void HAL_ETH_MspDeInit(ETH_HandleTypeDef* ethHandle)
{
  if(ethHandle->Instance==ETH)
  {
  /* USER CODE BEGIN ETH_MspDeInit 0 */

  /* USER CODE END ETH_MspDeInit 0 */
    /* Disable Peripheral clock */
    __HAL_RCC_ETH1MAC_CLK_DISABLE();
    __HAL_RCC_ETH1TX_CLK_DISABLE();
    __HAL_RCC_ETH1RX_CLK_DISABLE();

    /**ETH GPIO Configuration
    PC1     ------> ETH_MDC
    PA1     ------> ETH_REF_CLK
    PA2     ------> ETH_MDIO
    PA7     ------> ETH_CRS_DV
    PC4     ------> ETH_RXD0
    PC5     ------> ETH_RXD1
    PG11     ------> ETH_TX_EN
    PG13     ------> ETH_TXD0
    PG14     ------> ETH_TXD1
    */
    HAL_GPIO_DeInit(GPIOC, GPIO_PIN_1|GPIO_PIN_4|GPIO_PIN_5);

    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_7);

    HAL_GPIO_DeInit(GPIOG, GPIO_PIN_11|GPIO_PIN_13|GPIO_PIN_14);

  /* USER CODE BEGIN ETH_MspDeInit 1 */
    HAL_NVIC_DisableIRQ(ETH_IRQn);

  /* USER CODE END ETH_MspDeInit 1 */
  }
}

/*******************************************************************************
                       PHI IO Functions
*******************************************************************************/
/**
  * @brief  Initializes the MDIO interface GPIO and clocks.
  * @param  None
  * @retval 0 if OK, -1 if ERROR
  */
int32_t ETH_PHY_IO_Init(void)
{
  /* We assume that MDIO GPIO configuration is already done
     in the ETH_MspInit() else it should be done here
  */

  /* Configure the MDIO Clock */
  HAL_ETH_SetMDIOClockRange(&heth);

  /* Reset the on-board PHY exactly as in the verified DSO firmware. */
  {
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOI_CLK_ENABLE();
    GPIO_InitStruct.Pin = GPIO_PIN_3;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOI, &GPIO_InitStruct);

    HAL_GPIO_WritePin(GPIOI, GPIO_PIN_3, GPIO_PIN_RESET);
    HAL_Delay(10U);
    HAL_GPIO_WritePin(GPIOI, GPIO_PIN_3, GPIO_PIN_SET);
    HAL_Delay(50U);
  }

  return 0;
}

/**
  * @brief  De-Initializes the MDIO interface .
  * @param  None
  * @retval 0 if OK, -1 if ERROR
  */
int32_t ETH_PHY_IO_DeInit (void)
{
  return 0;
}

/**
  * @brief  Read a PHY register through the MDIO interface.
  * @param  DevAddr: PHY port address
  * @param  RegAddr: PHY register address
  * @param  pRegVal: pointer to hold the register value
  * @retval 0 if OK -1 if Error
  */
int32_t ETH_PHY_IO_ReadReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t *pRegVal)
{
  if(HAL_ETH_ReadPHYRegister(&heth, DevAddr, RegAddr, pRegVal) != HAL_OK)
  {
    return -1;
  }

  return 0;
}

/**
  * @brief  Write a value to a PHY register through the MDIO interface.
  * @param  DevAddr: PHY port address
  * @param  RegAddr: PHY register address
  * @param  RegVal: Value to be written
  * @retval 0 if OK -1 if Error
  */
int32_t ETH_PHY_IO_WriteReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t RegVal)
{
  if(HAL_ETH_WritePHYRegister(&heth, DevAddr, RegAddr, RegVal) != HAL_OK)
  {
    return -1;
  }

  return 0;
}

/**
  * @brief  Get the time in millisecons used for internal PHY driver process.
  * @retval Time value
  */
int32_t ETH_PHY_IO_GetTick(void)
{
  return HAL_GetTick();
}

/**
  * @brief  Check the ETH link state then update ETH driver and netif link accordingly.
  * @retval None
  */

void ethernet_link_thread(void const * argument)
{
  ETH_MACConfigTypeDef MACConf = {0};
  int32_t PHYLinkState = 0;
  uint32_t linkchanged = 0U, speed = 0U, duplex = 0U;
  uint32_t last_rx_irq_count = 0U;
  uint32_t last_tx_irq_count = 0U;
  uint32_t last_rx_irq_watchdog_count = 0U;
  uint32_t mac_idle_poll_count = 0U;
  uint32_t rx_idle_poll_count = 0U;
  uint32_t last_link_up = 0U;

  struct netif *netif = (struct netif *) argument;
/* USER CODE BEGIN ETH link init */

/* USER CODE END ETH link init */

  for(;;)
  {
  linkchanged = 0U;
  PHYLinkState = ETH_PHY_GetLinkStateCompat();
  eth_dbg.phy_link_state = PHYLinkState;
  eth_dbg.link_poll_count++;

  if(netif_is_link_up(netif) && (PHYLinkState <= LAN8742_STATUS_LINK_DOWN))
  {
    HAL_ETH_Stop_IT(&heth);
    netif_set_down(netif);
    netif_set_link_down(netif);
    eth_dbg.link_down_poll_count++;
  }
  else if(!netif_is_link_up(netif) && (PHYLinkState > LAN8742_STATUS_LINK_DOWN))
  {
    switch (PHYLinkState)
    {
    case LAN8742_STATUS_100MBITS_FULLDUPLEX:
      duplex = ETH_FULLDUPLEX_MODE;
      speed = ETH_SPEED_100M;
      linkchanged = 1;
      break;
    case LAN8742_STATUS_100MBITS_HALFDUPLEX:
      duplex = ETH_HALFDUPLEX_MODE;
      speed = ETH_SPEED_100M;
      linkchanged = 1;
      break;
    case LAN8742_STATUS_10MBITS_FULLDUPLEX:
      duplex = ETH_FULLDUPLEX_MODE;
      speed = ETH_SPEED_10M;
      linkchanged = 1;
      break;
    case LAN8742_STATUS_10MBITS_HALFDUPLEX:
      duplex = ETH_HALFDUPLEX_MODE;
      speed = ETH_SPEED_10M;
      linkchanged = 1;
      break;
    default:
      break;
    }

    if(linkchanged)
    {
      /* Get MAC Config MAC */
      HAL_ETH_GetMACConfig(&heth, &MACConf);
      MACConf.DuplexMode = duplex;
      MACConf.Speed = speed;
      HAL_ETH_SetMACConfig(&heth, &MACConf);
      HAL_ETH_Start_IT(&heth);
      eth_dbg.mac_restart_count++;
      netif_set_up(netif);
      netif_set_link_up(netif);
    }
  }
  else if(!netif_is_link_up(netif))
  {
    eth_dbg.link_down_poll_count++;
    if((eth_dbg.link_down_poll_count % 100U) == 0U)
    {
      (void)ETH_PHY_RestoreNormalMode();
      eth_dbg.link_recover_count++;
    }
  }
  else
  {
    eth_dbg.link_down_poll_count = 0U;
  }

  if(netif_is_link_up(netif))
  {
    if(last_link_up == 0U)
    {
      eth_dbg.link_flap_count++;
      last_link_up = 1U;
    }

    if((eth_dbg.rx_irq_count == last_rx_irq_count) &&
       (eth_dbg.tx_irq_count == last_tx_irq_count))
    {
      mac_idle_poll_count++;
      eth_dbg.mac_watchdog_count = mac_idle_poll_count;
      if(mac_idle_poll_count >= LAN8720_MAC_WATCHDOG_RESTART_POLLS)
      {
        (void)HAL_ETH_Stop_IT(&heth);
        (void)HAL_ETH_Start_IT(&heth);
        netif_set_up(netif);
        netif_set_link_up(netif);
        eth_dbg.mac_watchdog_restart_count++;
        mac_idle_poll_count = 0U;
      }
    }
    else
    {
      last_rx_irq_count = eth_dbg.rx_irq_count;
      last_tx_irq_count = eth_dbg.tx_irq_count;
      mac_idle_poll_count = 0U;
      eth_dbg.mac_watchdog_count = 0U;
    }

    if(eth_dbg.rx_irq_count == last_rx_irq_watchdog_count)
    {
      rx_idle_poll_count++;
      eth_dbg.rx_watchdog_count = rx_idle_poll_count;
      if(rx_idle_poll_count >= LAN8720_RX_WATCHDOG_RESTART_POLLS)
      {
        (void)HAL_ETH_Stop_IT(&heth);
        (void)HAL_ETH_Start_IT(&heth);
        netif_set_up(netif);
        netif_set_link_up(netif);
        eth_dbg.rx_watchdog_restart_count++;
        rx_idle_poll_count = 0U;
      }
    }
    else
    {
      last_rx_irq_watchdog_count = eth_dbg.rx_irq_count;
      rx_idle_poll_count = 0U;
      eth_dbg.rx_watchdog_count = 0U;
    }
  }
  else
  {
    if(last_link_up != 0U)
    {
      eth_dbg.link_flap_count++;
      last_link_up = 0U;
    }
    last_rx_irq_count = eth_dbg.rx_irq_count;
    last_tx_irq_count = eth_dbg.tx_irq_count;
    last_rx_irq_watchdog_count = eth_dbg.rx_irq_count;
    mac_idle_poll_count = 0U;
    rx_idle_poll_count = 0U;
    eth_dbg.mac_watchdog_count = 0U;
    eth_dbg.rx_watchdog_count = 0U;
  }

/* USER CODE BEGIN ETH link Thread core code for User BSP */
  ETH_DebugHandleCommand();
  eth_dbg.netif_up = netif_is_up(netif) ? 1U : 0U;
  eth_dbg.netif_link_up = netif_is_link_up(netif) ? 1U : 0U;

/* USER CODE END ETH link Thread core code for User BSP */

    osDelay(100);
  }
}

void HAL_ETH_RxAllocateCallback(uint8_t **buff)
{
/* USER CODE BEGIN HAL ETH RxAllocateCallback */
  struct pbuf_custom *p = LWIP_MEMPOOL_ALLOC(RX_POOL);
  if (p)
  {
    /* Get the buff from the struct pbuf address. */
    *buff = (uint8_t *)p + offsetof(RxBuff_t, buff);
    p->custom_free_function = pbuf_free_custom;
    /* Initialize the struct pbuf.
    * This must be performed whenever a buffer's allocated because it may be
    * changed by lwIP or the app, e.g., pbuf_free decrements ref. */
    pbuf_alloced_custom(PBUF_RAW, 0, PBUF_REF, p, *buff, ETH_RX_BUFFER_SIZE);
  }
  else
  {
    RxAllocStatus = RX_ALLOC_ERROR;
    eth_dbg.rx_alloc_error_count++;
    *buff = NULL;
  }
/* USER CODE END HAL ETH RxAllocateCallback */
}

void HAL_ETH_RxLinkCallback(void **pStart, void **pEnd, uint8_t *buff, uint16_t Length)
{
/* USER CODE BEGIN HAL ETH RxLinkCallback */

  struct pbuf **ppStart = (struct pbuf **)pStart;
  struct pbuf **ppEnd = (struct pbuf **)pEnd;
  struct pbuf *p = NULL;

  /* Get the struct pbuf from the buff address. */
  p = (struct pbuf *)(buff - offsetof(RxBuff_t, buff));
  p->next = NULL;
  p->tot_len = 0;
  p->len = Length;

  /* Chain the buffer. */
  if (!*ppStart)
  {
    /* The first buffer of the packet. */
    *ppStart = p;
  }
  else
  {
    /* Chain the buffer to the end of the packet. */
    (*ppEnd)->next = p;
  }
  *ppEnd  = p;

  /* Update the total length of all the buffers of the chain. Each pbuf in the chain should have its tot_len
   * set to its own length, plus the length of all the following pbufs in the chain. */
  for (p = *ppStart; p != NULL; p = p->next)
  {
    p->tot_len += Length;
  }

  /* Invalidate data cache because Rx DMA's writing to physical memory makes it stale. */
  {
    uint32_t cache_start = ((uint32_t)buff) & ~31U;
    uint32_t cache_end = (((uint32_t)buff) + Length + 31U) & ~31U;
    SCB_InvalidateDCache_by_Addr((uint32_t *)cache_start,
                                (int32_t)(cache_end - cache_start));
  }

/* USER CODE END HAL ETH RxLinkCallback */
}

void HAL_ETH_TxFreeCallback(uint32_t * buff)
{
/* USER CODE BEGIN HAL ETH TxFreeCallback */

  pbuf_free((struct pbuf *)buff);

/* USER CODE END HAL ETH TxFreeCallback */
}

/* USER CODE BEGIN 8 */

/* USER CODE END 8 */


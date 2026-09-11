#ifndef BSP_ETH_H
#define BSP_ETH_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define BSP_ETH_SERVER_IP0          192U
#define BSP_ETH_SERVER_IP1          168U
#define BSP_ETH_SERVER_IP2          100U
#define BSP_ETH_SERVER_IP3          100U
#define BSP_ETH_SERVER_PORT         5005U
#define BSP_ETH_PUBLISH_PERIOD_MS   100U

void Bsp_ETH_GetServerIp(uint8_t ip[4]);
uint16_t Bsp_ETH_GetServerPort(void);
uint32_t Bsp_ETH_GetPublishPeriodMs(void);

#ifdef __cplusplus
}
#endif

#endif /* BSP_ETH_H */

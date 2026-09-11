#include "Bsp_ETH.h"

void Bsp_ETH_GetServerIp(uint8_t ip[4])
{
    if (ip == 0)
    {
        return;
    }

    ip[0] = BSP_ETH_SERVER_IP0;
    ip[1] = BSP_ETH_SERVER_IP1;
    ip[2] = BSP_ETH_SERVER_IP2;
    ip[3] = BSP_ETH_SERVER_IP3;
}

uint16_t Bsp_ETH_GetServerPort(void)
{
    return (uint16_t)BSP_ETH_SERVER_PORT;
}

uint32_t Bsp_ETH_GetPublishPeriodMs(void)
{
    return (uint32_t)BSP_ETH_PUBLISH_PERIOD_MS;
}

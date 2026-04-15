#pragma once
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

void CfgUartBridge_Init(void);
void CfgUartBridge_Tick(void);
void CfgUartBridge_DelayMs(uint32_t ms);

#ifdef __cplusplus
}
#endif

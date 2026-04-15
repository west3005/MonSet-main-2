#include "stm32f4xx.h"

#if !defined (HSE_VALUE)
  #define HSE_VALUE    ((uint32_t)8000000U)
#endif
#if !defined (HSI_VALUE)
  #define HSI_VALUE    ((uint32_t)16000000U)
#endif

const uint8_t AHBPrescTable[16] = {
    0, 0, 0, 0, 0, 0, 0, 0,
    1, 2, 3, 4, 6, 7, 8, 9
};

const uint8_t APBPrescTable[8] = {
    0, 0, 0, 0, 1, 2, 3, 4
};

uint32_t SystemCoreClock = 16000000U;

#if !defined (VECT_TAB_OFFSET)
  #define VECT_TAB_OFFSET  0x00U
#endif

void SystemInit(void)
{
#if (__FPU_PRESENT == 1) && (__FPU_USED == 1)
    SCB->CPACR |= ((3UL << 10*2) | (3UL << 11*2));
#endif

    RCC->CR |= (uint32_t)0x00000001U;
    RCC->CFGR = 0x00000000U;
    RCC->CR &= (uint32_t)0xFEF6FFFFU;
    RCC->PLLCFGR = 0x24003010U;
    RCC->CR &= (uint32_t)0xFFFBFFFFU;
    RCC->CIR = 0x00000000U;

#ifdef VECT_TAB_SRAM
    SCB->VTOR = SRAM_BASE | VECT_TAB_OFFSET;
#else
    SCB->VTOR = FLASH_BASE | VECT_TAB_OFFSET;
#endif
}

void SystemCoreClockUpdate(void)
{
    uint32_t tmp = 0U, pllvco = 0U, pllp = 2U;
    uint32_t pllsource = 0U, pllm = 2U;

    tmp = RCC->CFGR & RCC_CFGR_SWS;

    switch (tmp)
    {
    case 0x00U:
        SystemCoreClock = HSI_VALUE;
        break;
    case 0x04U:
        SystemCoreClock = HSE_VALUE;
        break;
    case 0x08U:
        pllsource = (RCC->PLLCFGR & RCC_PLLCFGR_PLLSRC) >> 22U;
        pllm = RCC->PLLCFGR & RCC_PLLCFGR_PLLM;
        if (pllsource != 0U)
            pllvco = (HSE_VALUE / pllm) * ((RCC->PLLCFGR & RCC_PLLCFGR_PLLN) >> 6U);
        else
            pllvco = (HSI_VALUE / pllm) * ((RCC->PLLCFGR & RCC_PLLCFGR_PLLN) >> 6U);
        pllp = (((RCC->PLLCFGR & RCC_PLLCFGR_PLLP) >> 16U) + 1U) * 2U;
        SystemCoreClock = pllvco / pllp;
        break;
    default:
        SystemCoreClock = HSI_VALUE;
        break;
    }

    tmp = AHBPrescTable[((RCC->CFGR & RCC_CFGR_HPRE) >> 4U)];
    SystemCoreClock >>= tmp;
}

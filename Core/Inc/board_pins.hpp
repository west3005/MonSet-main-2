#pragma once

#include "stm32f4xx_hal.h"

// ================================================================
// Board GPIO map (единые имена, не зависят от CubeMX User Label)
// ================================================================

// --- Status / UI ---
#define PIN_STATUS_LED_PORT      GPIOC
#define PIN_STATUS_LED_PIN       GPIO_PIN_1

#define PIN_MODE_SW_PORT         GPIOC
#define PIN_MODE_SW_PIN          GPIO_PIN_0

// --- Net select (ETH/GSM) ---
#define PIN_NET_SELECT_PORT      GPIOB
#define PIN_NET_SELECT_PIN       GPIO_PIN_0

// --- W5500 ---
#define PIN_W5500_CS_PORT        GPIOA
#define PIN_W5500_CS_PIN         GPIO_PIN_8

#define PIN_W5500_RST_PORT       GPIOC
#define PIN_W5500_RST_PIN        GPIO_PIN_3

#define PIN_W5500_INT_PORT       GPIOC
#define PIN_W5500_INT_PIN        GPIO_PIN_4

// --- SD (если когда-то вернёшься к SPI-SD) ---
#define PIN_SD_CS_PORT           GPIOA
#define PIN_SD_CS_PIN            GPIO_PIN_4

// --- RS-485 (если DE реально используется — оставляем) ---
#define PIN_RS485_DE_PORT        GPIOB
#define PIN_RS485_DE_PIN         GPIO_PIN_12

// --- Cellular modem (UART2 + GPIO control) ---
#define PIN_CELL_PWR_EN_PORT     GPIOC
#define PIN_CELL_PWR_EN_PIN      GPIO_PIN_2

#define PIN_CELL_PWRKEY_PORT     GPIOD
#define PIN_CELL_PWRKEY_PIN      GPIO_PIN_6   // OD, active-low pulse to GND (через транзистор)

#define PIN_CELL_RESET_PORT      GPIOD
#define PIN_CELL_RESET_PIN       GPIO_PIN_7   // OD, active-low (через транзистор)

#define PIN_CELL_RI_WAKE_PORT    GPIOD
#define PIN_CELL_RI_WAKE_PIN     GPIO_PIN_8   // input, можно EXTI

#define PIN_CELL_STATUS_PORT     GPIOD
#define PIN_CELL_STATUS_PIN      GPIO_PIN_9   // input

#define PIN_CELL_DTR_PORT        GPIOD
#define PIN_CELL_DTR_PIN         GPIO_PIN_10  // output PP

#define PIN_GPIO_RESERVED1_PORT  GPIOD
#define PIN_GPIO_RESERVED1_PIN   GPIO_PIN_11

#define PIN_GPIO_RESERVED2_PORT  GPIOD
#define PIN_GPIO_RESERVED2_PIN   GPIO_PIN_12

// --- Iridium Edge (UART4 + GPIO) ---
#define PIN_EDGE_ON_PORT         GPIOD
#define PIN_EDGE_ON_PIN          GPIO_PIN_3   // OD (как на твоём скрине CubeMX)

#define PIN_EDGE_NET_AVAIL_PORT  GPIOD
#define PIN_EDGE_NET_AVAIL_PIN   GPIO_PIN_4   // input

#define PIN_EDGE_PWR_DET_PORT    GPIOD
#define PIN_EDGE_PWR_DET_PIN     GPIO_PIN_5   // input (после MAX3232)

// --- ESP (USART6 + GPIO) ---
#define PIN_ESP_EN_PORT          GPIOE
#define PIN_ESP_EN_PIN           GPIO_PIN_0

#define PIN_ESP_RST_PORT         GPIOE
#define PIN_ESP_RST_PIN          GPIO_PIN_1

#define PIN_ESP_GPIO0_PORT       GPIOE
#define PIN_ESP_GPIO0_PIN        GPIO_PIN_2

#pragma once

#include "stm32f4xx_hal.h"
#include <cstdint>

extern "C" {
#include "wizchip_conf.h"
#include "dhcp.h"
}

class W5500Net {
public:
  enum class Mode : uint8_t { Static = 0, Dhcp = 1 };

  bool init(SPI_HandleTypeDef* hspi, uint32_t dhcpTimeoutMs);
  void tick();

  bool ready() const { return m_ready; }
  Mode mode() const { return m_mode; }

  void getNetInfo(wiz_NetInfo& out) const;

private:
  void applyStatic();
  bool tryDhcp(uint32_t timeoutMs);

  static void dhcpCbAssigned();
  static void dhcpCbConflict();

  bool m_ready = false;
  Mode m_mode = Mode::Static;

  uint32_t m_lastDhcp1s = 0;
  uint8_t m_dhcpBuf[548]{};

  wiz_NetInfo m_info{};
};

/**
 * ================================================================
 * @file power_manager.hpp
 * @brief Управление энергосбережением (Stop Mode).
 * ================================================================
 */
#ifndef POWER_MANAGER_HPP
#define POWER_MANAGER_HPP

#include "main.h"
#include "config.hpp"
#include "sd_backup.hpp"

class PowerManager {
public:
    /**
     * @param hrtc — хэндл RTC (для Wakeup Timer)
     * @param backup — ссылка на SD backup (дeинит перед сном)
     */
    PowerManager(RTC_HandleTypeDef* hrtc, SdBackup& backup);

    /**
     * @brief Войти в Stop Mode на заданное время.
     *
     * Что происходит:
     * 1. SD деинит
     * 2. RTC Wakeup Timer → sec секунд
     * 3. SysTick стоп
     * 4. Stop Mode (LP Regulator)
     * 5. <--- СПИ --->
     * 6. SystemClock_Config()
     * 7. SysTick старт
     * 8. SD реинит
     *
     * @param sec — секунды до пробуждения
     */
    void enterStopMode(uint32_t sec);

private:
    RTC_HandleTypeDef* m_hrtc;
    SdBackup& m_backup;
};

#endif /* POWER_MANAGER_HPP */

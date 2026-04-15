/**
 * ================================================================
 * @file    captive_portal.hpp
 * @brief   Captive portal via ESP8266 AP mode.
 *
 * @note    When activated, ESP8266 starts as SoftAP and serves
 *          a setup wizard page. DNS requests are redirected to
 *          the AP IP. Supports simple mode (WiFi + APN + token)
 *          and advanced mode (all config params).
 * ================================================================
 */
#pragma once

#include "esp8266.hpp"
#include "runtime_config.hpp"
#include <cstdint>

class CaptivePortal {
public:
    CaptivePortal();

    /// @brief Initialize captive portal
    /// @param esp  ESP8266 driver instance
    void init(ESP8266* esp);

    /// @brief Start the captive portal (AP mode + HTTP server)
    /// @param apSsid  AP name (default: "MonSet-Setup")
    /// @param apPass  AP password (empty = open)
    /// @return true if started
    bool start(const char* apSsid = "MonSet-Setup", const char* apPass = "");

    /// @brief Stop the captive portal
    void stop();

    /// @brief Non-blocking tick — process incoming connections
    void tick();

    /// @brief Check if portal is active
    bool isActive() const { return m_active; }

    /// @brief Check if config was submitted and saved
    bool configSaved() const { return m_configSaved; }

private:
    ESP8266* m_esp     = nullptr;
    bool     m_active  = false;
    bool     m_configSaved = false;

    /// @brief Generate the setup wizard HTML page
    /// @param out   Output buffer
    /// @param outSz Buffer size
    /// @return HTML length
    int generateSetupPage(char* out, size_t outSz);

    /// @brief Parse submitted form data (URL-encoded)
    bool parseFormData(const char* body);

    /// @brief Handle incoming HTTP request
    void handleRequest(const char* data, uint16_t len);

    /// @brief URL-decode a string in-place
    static void urlDecode(char* str);

    /// @brief Find value of a form field
    static bool getFormField(const char* data, const char* key,
                              char* out, size_t outSz);
};

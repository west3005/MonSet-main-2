/**
 * ================================================================
 * @file web_server.hpp
 * @brief Minimal HTTP server on W5500 socket (port 80).
 *
 * Routes:
 *   GET  /                  → Config page (HTML, main page)
 *   GET  /config            → Config page (HTML)
 *   GET  /dashboard         → Dashboard (sensors, battery, channels)
 *   GET  /logs              → Log viewer (inline from RAM)
 *   GET  /test              → Test-send page
 *   GET  /api/sensors       → JSON sensor data
 *   GET  /api/channels      → JSON channel status
 *   GET  /api/config        → JSON current config
 *   GET  /api/web_mode      → JSON web-mode status
 *   GET  /api/test_result   → JSON last test result
 *   GET  /api/logs          → JSON log lines [?offset=N&count=M]
 *   GET  /api/logs/export   → Plain-text log dump
 *   POST /config            → Save config (JSON body)
 *   POST /api/config        → Save config (JSON body)
 *   POST /api/test_send     → Trigger manual send
 *   POST /api/logs/clear    → Clear RAM log buffer
 *
 * Basic Auth from RuntimeConfig (web_user, web_pass).
 * Non-blocking — call tick() from main loop.
 * ================================================================
 */
#pragma once

#include "runtime_config.hpp"
#include "sensor_reader.hpp"
#include "sd_backup.hpp"
#include <cstdint>

class BatteryMonitor;
class App;

class WebServer {
public:
    WebServer();

    void init(SensorReader* sensor, SdBackup* backup, BatteryMonitor* battery);
    void init(SensorReader* sensor, SdBackup* backup, BatteryMonitor* battery, App* app);

    void tick();
    bool isRunning() const { return m_running; }
    void setActivityCallback(void (*cb)(void*), void* ctx);

    // sendResponse is public so that helper lambdas / free functions can use it
    void sendResponse(uint8_t sn, int code, const char* contentType,
                      const char* body, uint16_t bodyLen);

private:
    SensorReader*   m_sensor  = nullptr;
    SdBackup*       m_backup  = nullptr;
    BatteryMonitor* m_battery = nullptr;
    App*            m_app     = nullptr;
    bool            m_running = false;

    void (*m_activityCb)(void*) = nullptr;
    void*  m_activityCtx        = nullptr;

    static constexpr uint8_t  HTTP_SOCKET = 5;
    static constexpr uint16_t HTTP_PORT   = 80;

    static constexpr uint16_t REQ_BUF_SIZE  = 1024;
    char m_reqBuf[REQ_BUF_SIZE];

    // 6 KB — вмещает любую inline HTML страницу (~3.5 KB max)
    static constexpr uint16_t RESP_BUF_SIZE = 6144;
    char m_respBuf[RESP_BUF_SIZE];

    bool checkAuth(const char* request);
    void send401(uint8_t sn);
    void send404(uint8_t sn);
    bool serveFile(uint8_t sn, const char* path, const char* contentType);

    void handleRequest(uint8_t sn, const char* request, uint16_t reqLen);

    // HTML pages
    void handleConfig(uint8_t sn);
    void handleIndex(uint8_t sn);
    void handleLogs(uint8_t sn);
    void handleTestPage(uint8_t sn);
    void handleExport(uint8_t sn);

    // JSON API
    void handleApiSensors(uint8_t sn);
    void handleApiConfig(uint8_t sn);
    void handleApiChannels(uint8_t sn);
    void handleApiWebMode(uint8_t sn);
    void handleApiTestSend(uint8_t sn);
    void handleApiTestResult(uint8_t sn);
    void handleApiLogs(uint8_t sn, const char* queryStr);
    void handleApiLogsExport(uint8_t sn);
    void handleApiLogsClear(uint8_t sn);

    // POST
    void handlePostConfig(uint8_t sn, const char* body);

    static int base64Decode(const char* in, uint8_t* out, int outMax);
};

/**
 * ================================================================
 * @file    web_server.hpp
 * @brief   Minimal HTTP server on W5500 socket (port 80).
 *
 * @note    Serves HTML pages from SD card (/www/ folder).
 *          Routes: GET /, GET /config, GET /logs, GET /export,
 *                  GET /api/sensors, GET /api/channels,
 *                  GET /api/web_mode, GET /api/test_result,
 *                  GET /api/logs[?offset=N&count=M],
 *                  GET /api/logs/export, GET /api/config,
 *                  POST /config, POST /api/test_send,
 *                  POST /api/logs/clear, POST /api/config.
 *          Basic Auth from RuntimeConfig (web_user, web_pass).
 *          Non-blocking — call tick() from main loop.
 * ================================================================
 */
#pragma once

#include "runtime_config.hpp"
#include "sensor_reader.hpp"
#include "sd_backup.hpp"
#include <cstdint>

// Forward declarations
class BatteryMonitor;
class App;

class WebServer {
public:
    WebServer();

    /// @brief Initialize web server
    /// @param sensor  Pointer to SensorReader for live data
    /// @param backup  Pointer to SdBackup for export
    /// @param battery Pointer to BatteryMonitor (can be nullptr)
    void init(SensorReader* sensor, SdBackup* backup, BatteryMonitor* battery);

    /// @brief Initialize web server (overload with App pointer)
    /// @param sensor  Pointer to SensorReader for live data
    /// @param backup  Pointer to SdBackup for export
    /// @param battery Pointer to BatteryMonitor (can be nullptr)
    /// @param app     Pointer to App instance for new API handlers
    void init(SensorReader* sensor, SdBackup* backup, BatteryMonitor* battery, App* app);

    /// @brief Non-blocking tick — process one pending request
    void tick();

    /// @brief Check if server is running
    bool isRunning() const { return m_running; }

    /**
     * @brief Set callback to notify App of web activity.
     *        Called on every valid HTTP request.
     * @param cb   Callback function pointer
     * @param ctx  Context pointer (App* cast to void*)
     */
    void setActivityCallback(void (*cb)(void*), void* ctx);

private:
    SensorReader*   m_sensor  = nullptr;
    SdBackup*       m_backup  = nullptr;
    BatteryMonitor* m_battery = nullptr;
    bool            m_running = false;

    void (*m_activityCb)(void*) = nullptr;
    void* m_activityCtx         = nullptr;
    App*  m_app                 = nullptr;  // for new API handlers

    /// Socket number for HTTP server (socket 5)
    static constexpr uint8_t HTTP_SOCKET = 5;
    static constexpr uint16_t HTTP_PORT  = 80;

    /// Request buffer
    static constexpr uint16_t REQ_BUF_SIZE = 1024;
    char m_reqBuf[REQ_BUF_SIZE];

    /// Response buffer
    static constexpr uint16_t RESP_BUF_SIZE = 2048;
    char m_respBuf[RESP_BUF_SIZE];

    /// @brief Check Basic Auth credentials
    bool checkAuth(const char* request);

    /// @brief Send HTTP response
    void sendResponse(uint8_t sn, int code, const char* contentType,
                      const char* body, uint16_t bodyLen);

    /// @brief Send 401 Unauthorized response
    void send401(uint8_t sn);

    /// @brief Send 404 Not Found
    void send404(uint8_t sn);

    /// @brief Route request to handler
    void handleRequest(uint8_t sn, const char* request, uint16_t reqLen);

    /// @brief Handlers
    void handleIndex(uint8_t sn);
    void handleConfig(uint8_t sn);
    void handleApiConfig(uint8_t sn);
    void handleLogs(uint8_t sn);
    void handleExport(uint8_t sn);
    void handleApiSensors(uint8_t sn);
    void handlePostConfig(uint8_t sn, const char* body);

    /// @brief New API handlers
    void handleApiChannels(uint8_t sn);
    void handleApiWebMode(uint8_t sn);
    void handleApiTestSend(uint8_t sn);
    void handleApiTestResult(uint8_t sn);
    void handleApiLogs(uint8_t sn, const char* queryStr);
    void handleApiLogsExport(uint8_t sn);
    void handleApiLogsClear(uint8_t sn);

    /// @brief Serve a file from SD /www/ folder
    bool serveFile(uint8_t sn, const char* path, const char* contentType);

    /// @brief Base64 decode (for Basic Auth)
    static int base64Decode(const char* in, uint8_t* out, int outMax);
};

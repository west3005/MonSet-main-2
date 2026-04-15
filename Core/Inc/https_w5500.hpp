#pragma once
#include <cstdint>

class HttpsW5500 {
public:
  // Возвращает HTTP status code (200/401/...), либо отрицательный код ошибки
  static int postJson(const char* httpsUrl,
                      const char* authBasicB64,   // без "Basic "
                      const char* json,
                      uint16_t jsonLen,
                      uint32_t timeoutMs);

  // Если хотите проверку сертификата по CA (PEM-строка), задайте не nullptr.
  // Если nullptr -> VERIFY_NONE (будет работать, но без криптопроверки сервера).
  static void setCaPem(const char* caPem);

private:
  struct UrlParts {
    char host[64];
    char path[128];
    uint16_t port;
  };

  static bool parseHttpsUrl(const char* url, UrlParts& out);
};

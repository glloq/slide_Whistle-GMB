/*
 * Minimal ESPAsyncWebServer surface used by core/platform/WebServerAdapter.h.
 */
#pragma once
#include <Arduino.h>
#include <FS.h>
#include <functional>

typedef int WebRequestMethodComposite;
#define HTTP_GET    0x01
#define HTTP_POST   0x02
#define HTTP_PUT    0x04
#define HTTP_DELETE 0x08
#define HTTP_ANY    0xff

enum AwsEventType { WS_EVT_CONNECT, WS_EVT_DISCONNECT, WS_EVT_DATA, WS_EVT_PONG, WS_EVT_ERROR };

class AsyncWebHeader {
public:
    String value() const { return String(""); }
};

class AsyncWebServerResponse {
public:
    void addHeader(const char*, const char*) {}
};

class AsyncWebServerRequest {
public:
    void* _tempObject = nullptr;
    WebRequestMethodComposite method() const { return HTTP_GET; }
    String url() const { return String("/"); }
    String contentType() const { return String(""); }
    bool hasHeader(const char*) const { return false; }
    AsyncWebHeader* getHeader(const char*) const { return nullptr; }
    void send(int, const char*, const char*) {}
    void send(AsyncWebServerResponse*) {}
    AsyncWebServerResponse* beginResponse(int, const char*, const char*) { return nullptr; }
};

class AsyncWebSocketClient {
public:
    uint32_t id() const { return 0; }
    void text(const char*) {}
};

class AsyncWebSocket {
public:
    explicit AsyncWebSocket(const char*) {}
    using EvHandler = std::function<void(AsyncWebSocket*, AsyncWebSocketClient*, AwsEventType,
                                         void*, uint8_t*, size_t)>;
    void onEvent(EvHandler) {}
    void cleanupClients() {}
    void textAll(const char*) {}
};

class AsyncStaticWebHandler {
public:
    AsyncStaticWebHandler& setDefaultFile(const char*) { return *this; }
};

class AsyncWebServer {
public:
    explicit AsyncWebServer(uint16_t) {}
    using ReqHandler  = std::function<void(AsyncWebServerRequest*)>;
    using BodyHandler = std::function<void(AsyncWebServerRequest*, uint8_t*, size_t, size_t, size_t)>;
    using UpHandler   = std::function<void(AsyncWebServerRequest*, const String&, size_t, uint8_t*, size_t, bool)>;
    void on(const char*, WebRequestMethodComposite, ReqHandler) {}
    void on(const char*, WebRequestMethodComposite, ReqHandler, UpHandler, BodyHandler) {}
    void onNotFound(ReqHandler) {}
    AsyncStaticWebHandler& serveStatic(const char*, FS&, const char*) {
        static AsyncStaticWebHandler h; return h;
    }
    void addHandler(AsyncWebSocket*) {}
    void begin() {}
};

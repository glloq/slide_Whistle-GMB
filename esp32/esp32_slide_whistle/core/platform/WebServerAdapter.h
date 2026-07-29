/*
 * core/platform/WebServerAdapter.h — thin bridge from ESPAsyncWebServer to the
 * portable ApiRouter. It only translates transport <-> ApiRequest/ApiReply and
 * pushes state over a WebSocket; all policy lives in the tested ApiRouter.
 *
 * Status: IMPLEMENTED (structure) · EXPERIMENTAL · NOT TESTED — REQUIRES HARDWARE
 */
#ifndef SWC_CORE_WEBSERVERADAPTER_H
#define SWC_CORE_WEBSERVERADAPTER_H
#if defined(ARDUINO)

#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include "../ApiRouter.h"

namespace swc {

class WebServerAdapter {
public:
    void begin(AsyncWebServer* server, AsyncWebSocket* ws, ApiRouter* router, uint32_t (*nowMs)()) {
        server_ = server; ws_ = ws; router_ = router; now_ = nowMs;

        // REST: one catch-all for /api/v1/*, body accumulated then dispatched.
        server_->on("/api/v1", HTTP_ANY,
            [this](AsyncWebServerRequest* r){ handle(r, nullptr, 0, 0, 0); },
            nullptr,
            [this](AsyncWebServerRequest* r, uint8_t* d, size_t len, size_t idx, size_t total){
                handle(r, d, len, idx, total);
            });
        // static web UI from LittleFS
        server_->serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

        // WebSocket: keyboard note events + differential status push.
        ws_->onEvent([this](AsyncWebSocket*, AsyncWebSocketClient* c, AwsEventType type,
                            void*, uint8_t* data, size_t len){
            if (type == WS_EVT_DATA) onWsData(c, data, len);
        });
        server_->addHandler(ws_);
    }

private:
    // Accumulate POST body across chunks, then dispatch once complete.
    void handle(AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t idx, size_t total) {
        if (total && data) {
            if (!r->_tempObject) r->_tempObject = new std::string();
            auto* body = static_cast<std::string*>(r->_tempObject);
            body->append(reinterpret_cast<char*>(data), len);
            if (idx + len < total) return;   // wait for the rest
        }
        ApiRequest req;
        req.method = methodName(r->method());
        req.path   = r->url().c_str();
        if (r->_tempObject) { req.body = *static_cast<std::string*>(r->_tempObject); }
        if (r->hasHeader("Origin"))       req.origin = r->getHeader("Origin")->value().c_str();
        if (r->hasHeader("X-Auth-Token")) req.token  = r->getHeader("X-Auth-Token")->value().c_str();
        if (r->contentType().length())    req.contentType = r->contentType().c_str();

        ApiReply rep = router_->handle(req, now_ ? now_() : 0);
        auto* resp = r->beginResponse(rep.status, "application/json", rep.body.c_str());
        resp->addHeader("Cache-Control", "no-store");
        r->send(resp);
        if (r->_tempObject) { delete static_cast<std::string*>(r->_tempObject); r->_tempObject = nullptr; }
    }

    void onWsData(AsyncWebSocketClient* c, uint8_t* data, size_t len) {
        // The WS frame is a small JSON command; reuse the same router path so a
        // web-keyboard NoteOn/NoteOff is authorised and queued identically.
        ApiRequest req;
        req.method = "POST"; req.path = "/api/v1/command"; req.contentType = "application/json";
        req.body.assign(reinterpret_cast<char*>(data), len);
        // a per-connection token is set at WS handshake (stored on the client)
        req.token = wsToken_;
        ApiReply rep = router_->handle(req, now_ ? now_() : 0);
        c->text(rep.body.c_str());
    }

    static const char* methodName(WebRequestMethodComposite m) {
        if (m == HTTP_GET) return "GET";
        if (m == HTTP_POST) return "POST";
        if (m == HTTP_PUT) return "PUT";
        if (m == HTTP_DELETE) return "DELETE";
        return "GET";
    }

    AsyncWebServer*  server_ = nullptr;
    AsyncWebSocket*  ws_ = nullptr;
    ApiRouter*       router_ = nullptr;
    uint32_t (*now_)() = nullptr;
    std::string      wsToken_;   // set after WS auth; TODO: per-client tokens
public:
    void setWsToken(const std::string& t) { wsToken_ = t; }
    AsyncWebSocket* ws() { return ws_; }
};

} // namespace swc

#endif // ARDUINO
#endif // SWC_CORE_WEBSERVERADAPTER_H

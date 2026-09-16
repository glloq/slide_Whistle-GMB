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
#include <map>
#include "../ApiRouter.h"
#include "../AuthManager.h"

namespace swc {

class WebServerAdapter {
public:
    void begin(AsyncWebServer* server, AsyncWebSocket* ws, ApiRouter* router,
               AuthManager* auth, uint32_t (*nowMs)()) {
        server_ = server; ws_ = ws; router_ = router; auth_ = auth; now_ = nowMs;

        // REST: register EACH concrete route (ESPAsyncWebServer matches exact
        // paths, not prefixes — a single "/api/v1" would never match the
        // sub-routes, review item #4). Every route shares the same dispatcher
        // and body accumulator.
        static const char* kRoutes[] = {
            "/api/v1/status", "/api/v1/config", "/api/v1/preset",
            "/api/v1/command", "/api/v1/session", "/api/v1/factory-reset",
            "/api/v1/restart",
        };
        for (const char* path : kRoutes) {
            server_->on(path, HTTP_ANY,
                [this](AsyncWebServerRequest* r){ handle(r, nullptr, 0, 0, 0); },
                nullptr,
                [this](AsyncWebServerRequest* r, uint8_t* d, size_t len, size_t idx, size_t total){
                    handle(r, d, len, idx, total);
                });
        }
        // Any other /api/v1/* URI gets a clean 404 envelope instead of the
        // static handler swallowing it.
        server_->onNotFound([this](AsyncWebServerRequest* r){
            if (String(r->url()).startsWith("/api/")) {
                r->send(404, "application/json",
                        "{\"ok\":false,\"error\":{\"code\":\"NOT_FOUND\",\"message\":\"unknown route\"}}");
            } else {
                r->send(404, "text/plain", "not found");
            }
        });
        // static web UI from LittleFS
        server_->serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

        // WebSocket: per-client session + keyboard note events.
        ws_->onEvent([this](AsyncWebSocket*, AsyncWebSocketClient* c, AwsEventType type,
                            void*, uint8_t* data, size_t len){
            if (type == WS_EVT_DISCONNECT) { wsTokens_.erase(c->id()); }
            else if (type == WS_EVT_DATA)  { onWsData(c, data, len); }
        });
        server_->addHandler(ws_);
    }

private:
    // Accumulate POST body across chunks, then dispatch once complete.
    void handle(AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t idx, size_t total) {
        // §6: refuse an oversized body BEFORE buffering it. On the first chunk
        // `total` is the declared Content-Length; guard the running size too so a
        // chunked upload with no length can't grow the heap past the cap either.
        const size_t cap = router_ ? router_->maxBodyBytes() : 16384;
        const size_t projected = (total ? total : idx + len);
        if (httpBodyExceedsLimit(projected, cap)) {
            if (r->_tempObject) { delete static_cast<std::string*>(r->_tempObject); r->_tempObject = nullptr; }
            r->send(413, "application/json",
                    "{\"ok\":false,\"error\":{\"code\":\"BODY_TOO_LARGE\",\"message\":\"request body exceeds limit\"}}");
            return;
        }
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
        std::string frame(reinterpret_cast<char*>(data), len);
        // A first frame {"auth":"<session>"} associates THIS client id with a
        // session token (review item #6 — per-client, not one global token).
        JsonValue j;
        if (jsonParse(frame, j, nullptr) && j.has("auth")) {
            // VERIFY the session before confirming — do not report authed:true
            // for an invalid/expired token (review #34).
            std::string tok = j.str_or("auth", "");
            bool okAuth = auth_ && auth_->verifySession(tok, now_ ? now_() : 0);
            if (okAuth) { wsTokens_[c->id()] = tok; c->text("{\"ok\":true,\"data\":{\"authed\":true}}"); }
            else        { wsTokens_.erase(c->id()); c->text("{\"ok\":false,\"error\":{\"code\":\"UNAUTHORIZED\"}}"); }
            return;
        }
        // Otherwise it's a command; attach this client's stored token and reuse
        // the same authorised, rate-limited router path as REST.
        ApiRequest req;
        req.method = "POST"; req.path = "/api/v1/command"; req.contentType = "application/json";
        req.body = frame;
        auto it = wsTokens_.find(c->id());
        req.token = (it != wsTokens_.end()) ? it->second : std::string();
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
    AuthManager*     auth_ = nullptr;
    uint32_t (*now_)() = nullptr;
    std::map<uint32_t, std::string> wsTokens_;   // WS client id → session token
public:
    AsyncWebSocket* ws() { return ws_; }
};

} // namespace swc

#endif // ARDUINO
#endif // SWC_CORE_WEBSERVERADAPTER_H

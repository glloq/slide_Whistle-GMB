/*
 * core/ApiResponse.h — the single, consistent API response envelope (Section 14).
 *
 *   { "ok": true,  "data": { ... } }
 *   { "ok": false, "error": { "code": "GPIO_CONFLICT",
 *                             "message": "GPIO22 is already used by I2C SCL",
 *                             "field": "instruments[1].air.pump.pin" } }
 *
 * Every endpoint returns this shape so the frontend can never mistake an HTTP
 * 400/500 body for success (correction #16/#23). A helper turns validator
 * issues straight into this envelope.
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_APIRESPONSE_H
#define SWC_CORE_APIRESPONSE_H

#include "Json.h"
#include "HardwareValidator.h"

namespace swc {

struct ApiError { std::string code, message, field; };

inline std::string apiOk(const JsonValue& data) {
    JsonValue r = JsonValue::makeObj();
    r.set("ok", true);
    r.set("data", data);
    return jsonDump(r);
}
inline std::string apiOk() { return apiOk(JsonValue::makeObj()); }

inline std::string apiErr(const std::string& code, const std::string& message,
                          const std::string& field = "") {
    JsonValue r = JsonValue::makeObj();
    r.set("ok", false);
    JsonValue e = JsonValue::makeObj();
    e.set("code", code);
    e.set("message", message);
    if (!field.empty()) e.set("field", field);
    r.set("error", e);
    return jsonDump(r);
}

// Turn the first hard error from a validation run into an error envelope, or an
// ok envelope carrying any warnings, so a config apply reports precisely.
inline std::string apiFromValidation(const std::vector<ValidationIssue>& issues) {
    for (const auto& i : issues)
        if (i.severity == Severity::Error)
            return apiErr(i.code, i.message, i.field);
    JsonValue data = JsonValue::makeObj();
    JsonValue warns = JsonValue::makeArr();
    for (const auto& i : issues) {
        JsonValue w = JsonValue::makeObj();
        w.set("code", i.code); w.set("message", i.message); w.set("field", i.field);
        warns.arr.push_back(w);
    }
    data.set("warnings", warns);
    return apiOk(data);
}

} // namespace swc

#endif // SWC_CORE_APIRESPONSE_H

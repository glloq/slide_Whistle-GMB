/*
 * core/Json.h — tiny portable JSON value + parser + serializer.
 *
 * Dependency-free (no ArduinoJson) so config import/export and migration are
 * unit-tested natively. On the ESP32 the same code runs; the firmware may still
 * use ArduinoJson at the HTTP edge, but the canonical config round-trip lives
 * here where it can be tested with quotes / backslashes / UTF-8 (Section 18).
 *
 * Not a maximal JSON implementation — it covers objects, arrays, strings
 * (with escapes), numbers, bool and null, which is all the config needs.
 *
 * Status: IMPLEMENTED / TESTED IN SOFTWARE
 */
#ifndef SWC_CORE_JSON_H
#define SWC_CORE_JSON_H

#include <string>
#include <vector>
#include <utility>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cctype>
#include <cstring>

namespace swc {

struct JsonValue {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool        b = false;
    double      num = 0;
    std::string str;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;   // insertion-ordered

    static JsonValue makeObj() { JsonValue v; v.type = Obj; return v; }
    static JsonValue makeArr() { JsonValue v; v.type = Arr; return v; }

    // object helpers
    bool has(const std::string& k) const {
        for (auto& kv : obj) if (kv.first == k) return true;
        return false;
    }
    const JsonValue* find(const std::string& k) const {
        for (auto& kv : obj) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    JsonValue& set(const std::string& k, JsonValue v) {
        for (auto& kv : obj) if (kv.first == k) { kv.second = std::move(v); return kv.second; }
        obj.emplace_back(k, std::move(v));
        type = Obj;
        return obj.back().second;
    }
    void set(const std::string& k, double n)             { JsonValue v; v.type = Num; v.num = n; set(k, v); }
    void set(const std::string& k, int n)                { set(k, (double)n); }
    void set(const std::string& k, bool bv)              { JsonValue v; v.type = Bool; v.b = bv; set(k, v); }
    void set(const std::string& k, const std::string& s) { JsonValue v; v.type = Str; v.str = s; set(k, v); }
    void set(const std::string& k, const char* s)        { set(k, std::string(s)); }

    // typed getters with defaults
    double asNum(double d = 0) const { return type == Num ? num : (type == Bool ? (b?1:0) : d); }
    long   asInt(long d = 0) const { return type == Num ? (long)num : d; }
    bool   asBool(bool d = false) const { return type == Bool ? b : (type == Num ? num != 0 : d); }
    std::string asStr(const std::string& d = "") const { return type == Str ? str : d; }

    double num_or(const std::string& k, double d) const { auto* v = find(k); return v ? v->asNum(d) : d; }
    long   int_or(const std::string& k, long d) const { auto* v = find(k); return v ? v->asInt(d) : d; }
    bool   bool_or(const std::string& k, bool d) const { auto* v = find(k); return v ? v->asBool(d) : d; }
    std::string str_or(const std::string& k, const std::string& d) const { auto* v = find(k); return v ? v->asStr(d) : d; }
};

// --- serialization ---------------------------------------------------------
inline void jsonEscape(const std::string& s, std::string& out) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", c); out += buf; }
                else out.push_back((char)c);   // UTF-8 bytes pass through verbatim
        }
    }
    out.push_back('"');
}

inline void jsonWrite(const JsonValue& v, std::string& out) {
    switch (v.type) {
        case JsonValue::Null: out += "null"; break;
        case JsonValue::Bool: out += v.b ? "true" : "false"; break;
        case JsonValue::Num: {
            char buf[32];
            if (v.num == (long long)v.num) std::snprintf(buf, sizeof(buf), "%lld", (long long)v.num);
            else                           std::snprintf(buf, sizeof(buf), "%.6g", v.num);
            out += buf; break;
        }
        case JsonValue::Str: jsonEscape(v.str, out); break;
        case JsonValue::Arr:
            out.push_back('[');
            for (size_t i = 0; i < v.arr.size(); ++i) { if (i) out.push_back(','); jsonWrite(v.arr[i], out); }
            out.push_back(']'); break;
        case JsonValue::Obj:
            out.push_back('{');
            for (size_t i = 0; i < v.obj.size(); ++i) {
                if (i) out.push_back(',');
                jsonEscape(v.obj[i].first, out); out.push_back(':'); jsonWrite(v.obj[i].second, out);
            }
            out.push_back('}'); break;
    }
}

inline std::string jsonDump(const JsonValue& v) { std::string s; jsonWrite(v, s); return s; }

// --- parser (recursive descent) --------------------------------------------
class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}
    bool parse(JsonValue& out) { skip(); bool ok = value(out); skip(); return ok && i_ >= s_.size(); }
    const std::string& error() const { return err_; }

private:
    bool value(JsonValue& v) {
        skip();
        if (i_ >= s_.size()) return fail("unexpected end");
        char c = s_[i_];
        if (c == '{') return object(v);
        if (c == '[') return array(v);
        if (c == '"') { v.type = JsonValue::Str; return string(v.str); }
        if (c == 't' || c == 'f') return boolean(v);
        if (c == 'n') return null(v);
        return number(v);
    }
    bool object(JsonValue& v) {
        v.type = JsonValue::Obj; ++i_; skip();
        if (peek('}')) { ++i_; return true; }
        for (;;) {
            skip();
            if (i_ >= s_.size() || s_[i_] != '"') return fail("expected key");
            std::string key; if (!string(key)) return false;
            skip(); if (!peek(':')) return fail("expected ':'"); ++i_;
            JsonValue child; if (!value(child)) return false;
            v.obj.emplace_back(key, std::move(child));
            skip();
            if (peek(',')) { ++i_; continue; }
            if (peek('}')) { ++i_; return true; }
            return fail("expected ',' or '}'");
        }
    }
    bool array(JsonValue& v) {
        v.type = JsonValue::Arr; ++i_; skip();
        if (peek(']')) { ++i_; return true; }
        for (;;) {
            JsonValue child; if (!value(child)) return false;
            v.arr.push_back(std::move(child));
            skip();
            if (peek(',')) { ++i_; continue; }
            if (peek(']')) { ++i_; return true; }
            return fail("expected ',' or ']'");
        }
    }
    bool string(std::string& out) {
        ++i_;   // opening quote
        while (i_ < s_.size()) {
            char c = s_[i_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (i_ >= s_.size()) return fail("bad escape");
                char e = s_[i_++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (i_ + 4 > s_.size()) return fail("bad \\u");
                        unsigned code = (unsigned)strtol(s_.substr(i_, 4).c_str(), nullptr, 16);
                        i_ += 4;
                        // minimal UTF-8 encode of the BMP code point
                        if (code < 0x80) out.push_back((char)code);
                        else if (code < 0x800) {
                            out.push_back((char)(0xC0 | (code >> 6)));
                            out.push_back((char)(0x80 | (code & 0x3F)));
                        } else {
                            out.push_back((char)(0xE0 | (code >> 12)));
                            out.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
                            out.push_back((char)(0x80 | (code & 0x3F)));
                        }
                        break;
                    }
                    default: return fail("bad escape char");
                }
            } else out.push_back(c);
        }
        return fail("unterminated string");
    }
    bool number(JsonValue& v) {
        size_t start = i_;
        while (i_ < s_.size() && (isdigit((unsigned char)s_[i_]) || strchr("+-.eE", s_[i_]))) ++i_;
        if (i_ == start) return fail("invalid number");
        v.type = JsonValue::Num; v.num = strtod(s_.substr(start, i_ - start).c_str(), nullptr);
        return true;
    }
    bool boolean(JsonValue& v) {
        if (s_.compare(i_, 4, "true") == 0) { v.type = JsonValue::Bool; v.b = true; i_ += 4; return true; }
        if (s_.compare(i_, 5, "false") == 0) { v.type = JsonValue::Bool; v.b = false; i_ += 5; return true; }
        return fail("invalid literal");
    }
    bool null(JsonValue& v) {
        if (s_.compare(i_, 4, "null") == 0) { v.type = JsonValue::Null; i_ += 4; return true; }
        return fail("invalid literal");
    }
    void skip() { while (i_ < s_.size() && (s_[i_]==' '||s_[i_]=='\t'||s_[i_]=='\n'||s_[i_]=='\r')) ++i_; }
    bool peek(char c) const { return i_ < s_.size() && s_[i_] == c; }
    bool fail(const char* m) { if (err_.empty()) err_ = m; return false; }

    const std::string& s_;
    size_t i_ = 0;
    std::string err_;
};

inline bool jsonParse(const std::string& text, JsonValue& out, std::string* err = nullptr) {
    JsonParser p(text);
    bool ok = p.parse(out);
    if (!ok && err) *err = p.error();
    return ok;
}

// FNV-1a checksum for integrity checks (Section 10).
inline uint32_t fnv1a(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}

} // namespace swc

#endif // SWC_CORE_JSON_H

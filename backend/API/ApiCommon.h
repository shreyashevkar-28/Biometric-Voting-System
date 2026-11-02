#pragma once
#include <cerrno>
#include <cstdlib>
#include <string>

#include "../Common/Util.h"
#include "../Services/AuthService.h"
#include "httplib.h"

namespace bvs {

inline void sendJson(httplib::Response& res, int status, const std::string& body) {
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_header("X-Content-Type-Options", "nosniff");
    res.set_content(body, "application/json");
}

inline void sendError(httplib::Response& res, int status, const std::string& msg) {
    sendJson(res, status, "{\"error\":" + jsonStr(msg) + "}");
}

// Admin endpoints require the X-Admin-Token header (constant-time compare).
inline bool requireAdmin(const httplib::Request& req, httplib::Response& res, const AuthService& auth) {
    if (!auth.isAdmin(req.get_header_value("X-Admin-Token"))) {
        sendError(res, 401, "unauthorized");
        return false;
    }
    return true;
}

// Extract string from URL query params or fallback to simple flat JSON body key
inline std::string paramStr(const httplib::Request& req, const char* name, const std::string& def = "") {
    if (req.has_param(name)) return req.get_param_value(name);
    if (!req.body.empty()) {
        std::string key = std::string("\"") + name + "\"";
        auto pos = req.body.find(key);
        if (pos != std::string::npos) {
            pos = req.body.find(':', pos + key.size());
            if (pos != std::string::npos) {
                while (pos + 1 < req.body.size() && (req.body[pos + 1] == ' ' || req.body[pos + 1] == '\t')) ++pos;
                if (pos + 1 < req.body.size() && req.body[pos + 1] == '"') {
                    size_t start = pos + 2;
                    size_t end = req.body.find('"', start);
                    if (end != std::string::npos) return req.body.substr(start, end - start);
                }
            }
        }
    }
    return def;
}

inline int64_t paramInt(const httplib::Request& req, const char* name, int64_t def) {
    if (req.has_param(name)) {
        const std::string v = req.get_param_value(name);
        if (v.empty() || v.size() > 18) return def;
        char* end = nullptr;
        errno = 0;
        long long x = std::strtoll(v.c_str(), &end, 10);
        return (errno == 0 && end && *end == '\0') ? x : def;
    }
    if (!req.body.empty()) {
        std::string key = std::string("\"") + name + "\"";
        auto pos = req.body.find(key);
        if (pos != std::string::npos) {
            pos = req.body.find(':', pos + key.size());
            if (pos != std::string::npos) {
                size_t start = pos + 1;
                while (start < req.body.size() && (req.body[start] == ' ' || req.body[start] == '"')) ++start;
                char* end = nullptr;
                errno = 0;
                long long x = std::strtoll(req.body.c_str() + start, &end, 10);
                if (errno == 0 && end != req.body.c_str() + start) return x;
            }
        }
    }
    return def;
}

inline size_t paramLimit(const httplib::Request& req, const char* name, size_t def, size_t maxv) {
    int64_t v = paramInt(req, name, static_cast<int64_t>(def));
    if (v < 1) v = 1;
    return static_cast<size_t>(v) > maxv ? maxv : static_cast<size_t>(v);
}

// Escapes single quotes for safe SQLite string literals
inline std::string sqlQuote(const std::string& s) {
    std::string out = "'";
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '\'') out += "''";
        else if (c >= 32 && c != 127) out += c;
    }
    out += "'";
    return out;
}

}  // namespace bvs
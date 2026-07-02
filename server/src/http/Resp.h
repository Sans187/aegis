#pragma once
// 控制器共用：JSON 响应与查询参数读取。

#include <drogon/drogon.h>
#include <optional>
#include <string>

namespace aegis::http {

inline drogon::HttpResponsePtr jsonResp(const Json::Value& v,
                                        drogon::HttpStatusCode code = drogon::k200OK) {
    auto r = drogon::HttpResponse::newHttpJsonResponse(v);
    r->setStatusCode(code);
    return r;
}

inline drogon::HttpResponsePtr err(const std::string& msg, drogon::HttpStatusCode code) {
    Json::Value v; v["ok"] = false; v["error"] = msg;
    return jsonResp(v, code);
}

inline drogon::HttpResponsePtr okResp() {
    Json::Value v; v["ok"] = true;
    return jsonResp(v);
}

// 查询参数 -> optional：缺失或空串返回 nullopt（用于 SQL 的 NULL 开关）。
inline std::optional<std::string> optParam(const drogon::HttpRequestPtr& req, const std::string& key) {
    auto m = req->getParameters();
    auto it = m.find(key);
    if (it == m.end() || it->second.empty()) return std::nullopt;
    return it->second;
}

inline std::string param(const drogon::HttpRequestPtr& req, const std::string& key,
                         const std::string& def = "") {
    auto m = req->getParameters();
    auto it = m.find(key);
    return (it == m.end()) ? def : it->second;
}

} // namespace aegis::http

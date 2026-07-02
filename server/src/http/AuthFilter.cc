#include "http/AuthFilter.h"
#include "http/Session.h"

#include <drogon/drogon.h>

using namespace drogon;

namespace aegis {

static std::string bearerToken(const HttpRequestPtr& req) {
    const std::string& h = req->getHeader("authorization");
    const std::string prefix = "Bearer ";
    if (h.size() > prefix.size() && h.compare(0, prefix.size(), prefix) == 0)
        return h.substr(prefix.size());
    return {};
}

void AuthFilter::doFilter(const HttpRequestPtr& req,
                          FilterCallback&& fcb,
                          FilterChainCallback&& fccb) {
    const std::string token = bearerToken(req);
    if (token.empty()) {
        Json::Value v; v["ok"] = false; v["error"] = "missing token";
        auto resp = HttpResponse::newHttpJsonResponse(v);
        resp->setStatusCode(k401Unauthorized);
        return fcb(resp);
    }

    // 异步校验会话，避免阻塞事件循环。
    auto fcbPtr  = std::make_shared<FilterCallback>(std::move(fcb));
    auto fccbPtr = std::make_shared<FilterChainCallback>(std::move(fccb));
    drogon::async_run([req, token, fcbPtr, fccbPtr]() -> drogon::Task<> {
        auto agentId = co_await aegis::http::validateSession(token);
        if (!agentId) {
            Json::Value v; v["ok"] = false; v["error"] = "invalid or expired session";
            auto resp = HttpResponse::newHttpJsonResponse(v);
            resp->setStatusCode(k401Unauthorized);
            (*fcbPtr)(resp);
            co_return;
        }
        req->getAttributes()->insert("uid", *agentId);
        (*fccbPtr)();
        co_return;
    });
}

} // namespace aegis

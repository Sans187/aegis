#pragma once
#include <drogon/HttpController.h>

namespace aegis {

class SecurityController : public drogon::HttpController<SecurityController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(SecurityController::status, "/api/security/status", drogon::Get,  "aegis::AuthFilter");
    ADD_METHOD_TO(SecurityController::set,    "/api/security/set",    drogon::Post, "aegis::AuthFilter");
    METHOD_LIST_END

    drogon::Task<> status(drogon::HttpRequestPtr, std::function<void(const drogon::HttpResponsePtr&)>);
    drogon::Task<> set(drogon::HttpRequestPtr, std::function<void(const drogon::HttpResponsePtr&)>);
};

} // namespace aegis

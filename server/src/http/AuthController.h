#pragma once
#include <drogon/HttpController.h>

namespace aegis {

class AuthController : public drogon::HttpController<AuthController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AuthController::login,          "/api/auth/login",           drogon::Post);
    ADD_METHOD_TO(AuthController::logout,         "/api/auth/logout",          drogon::Post, "aegis::AuthFilter");
    ADD_METHOD_TO(AuthController::me,             "/api/auth/me",              drogon::Get,  "aegis::AuthFilter");
    ADD_METHOD_TO(AuthController::changePassword, "/api/auth/change-password", drogon::Post, "aegis::AuthFilter");
    METHOD_LIST_END

    drogon::Task<> login(drogon::HttpRequestPtr req,
                         std::function<void(const drogon::HttpResponsePtr&)> cb);
    drogon::Task<> logout(drogon::HttpRequestPtr req,
                          std::function<void(const drogon::HttpResponsePtr&)> cb);
    drogon::Task<> me(drogon::HttpRequestPtr req,
                      std::function<void(const drogon::HttpResponsePtr&)> cb);
    drogon::Task<> changePassword(drogon::HttpRequestPtr req,
                                  std::function<void(const drogon::HttpResponsePtr&)> cb);
};

} // namespace aegis

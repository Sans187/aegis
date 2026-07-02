#pragma once
#include <drogon/HttpController.h>

namespace aegis {

class UserController : public drogon::HttpController<UserController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(UserController::list,   "/api/users",        drogon::Get,  "aegis::AuthFilter");
    ADD_METHOD_TO(UserController::batch,  "/api/users/batch",  drogon::Post, "aegis::AuthFilter");
    ADD_METHOD_TO(UserController::detail, "/api/users/detail", drogon::Get,  "aegis::AuthFilter");
    ADD_METHOD_TO(UserController::create, "/api/users/create", drogon::Post, "aegis::AuthFilter");
    ADD_METHOD_TO(UserController::freeze, "/api/users/freeze", drogon::Post, "aegis::AuthFilter");
    ADD_METHOD_TO(UserController::extend, "/api/users/extend", drogon::Post, "aegis::AuthFilter");
    ADD_METHOD_TO(UserController::unbind, "/api/users/unbind", drogon::Post, "aegis::AuthFilter");
    ADD_METHOD_TO(UserController::remove, "/api/users/delete", drogon::Post, "aegis::AuthFilter");
    ADD_METHOD_TO(UserController::batchOp, "/api/users/batch-op", drogon::Post, "aegis::AuthFilter");
    METHOD_LIST_END

    drogon::Task<> list(drogon::HttpRequestPtr req,
                        std::function<void(const drogon::HttpResponsePtr&)> cb);
    drogon::Task<> batch(drogon::HttpRequestPtr req,
                         std::function<void(const drogon::HttpResponsePtr&)> cb);
    drogon::Task<> detail(drogon::HttpRequestPtr req,
                          std::function<void(const drogon::HttpResponsePtr&)> cb);
    drogon::Task<> create(drogon::HttpRequestPtr req,
                          std::function<void(const drogon::HttpResponsePtr&)> cb);
    drogon::Task<> freeze(drogon::HttpRequestPtr req,
                          std::function<void(const drogon::HttpResponsePtr&)> cb);
    drogon::Task<> extend(drogon::HttpRequestPtr req,
                          std::function<void(const drogon::HttpResponsePtr&)> cb);
    drogon::Task<> unbind(drogon::HttpRequestPtr req,
                          std::function<void(const drogon::HttpResponsePtr&)> cb);
    drogon::Task<> remove(drogon::HttpRequestPtr req,
                          std::function<void(const drogon::HttpResponsePtr&)> cb);
    drogon::Task<> batchOp(drogon::HttpRequestPtr req,
                           std::function<void(const drogon::HttpResponsePtr&)> cb);
};

} // namespace aegis

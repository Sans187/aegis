#pragma once
#include <drogon/HttpController.h>

namespace aegis {

class AppController : public drogon::HttpController<AppController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AppController::list,           "/api/apps",          drogon::Get,  "aegis::AuthFilter");
    ADD_METHOD_TO(AppController::create,         "/api/apps/create",   drogon::Post, "aegis::AuthFilter");
    ADD_METHOD_TO(AppController::remove,         "/api/apps/delete",   drogon::Post, "aegis::AuthFilter");
    ADD_METHOD_TO(AppController::rename,         "/api/apps/rename",   drogon::Post, "aegis::AuthFilter");
    ADD_METHOD_TO(AppController::getSettings,     "/api/apps/settings", drogon::Get,  "aegis::AuthFilter");
    ADD_METHOD_TO(AppController::updateSettings,  "/api/apps/settings", drogon::Post, "aegis::AuthFilter");
    METHOD_LIST_END

    drogon::Task<> list(drogon::HttpRequestPtr req,
                        std::function<void(const drogon::HttpResponsePtr&)> cb);
    drogon::Task<> create(drogon::HttpRequestPtr req,
                          std::function<void(const drogon::HttpResponsePtr&)> cb);
    drogon::Task<> remove(drogon::HttpRequestPtr req,
                          std::function<void(const drogon::HttpResponsePtr&)> cb);
    drogon::Task<> rename(drogon::HttpRequestPtr req,
                          std::function<void(const drogon::HttpResponsePtr&)> cb);
    drogon::Task<> getSettings(drogon::HttpRequestPtr req,
                               std::function<void(const drogon::HttpResponsePtr&)> cb);
    drogon::Task<> updateSettings(drogon::HttpRequestPtr req,
                                  std::function<void(const drogon::HttpResponsePtr&)> cb);
};

} // namespace aegis

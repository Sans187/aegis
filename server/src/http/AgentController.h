#pragma once
#include <drogon/HttpController.h>
#include <drogon/orm/DbClient.h>
#include <memory>
#include <string>

namespace aegis {

class AgentController : public drogon::HttpController<AgentController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(AgentController::list,          "/api/agents",                drogon::Get,  "aegis::AuthFilter");
    ADD_METHOD_TO(AgentController::subtree,       "/api/agents/subtree",        drogon::Get,  "aegis::AuthFilter");
    ADD_METHOD_TO(AgentController::create,        "/api/agents",                drogon::Post, "aegis::AuthFilter");
    ADD_METHOD_TO(AgentController::update,        "/api/agents/update",         drogon::Post, "aegis::AuthFilter");
    ADD_METHOD_TO(AgentController::setStatus,     "/api/agents/status",         drogon::Post, "aegis::AuthFilter");
    ADD_METHOD_TO(AgentController::resetPassword, "/api/agents/reset-password", drogon::Post, "aegis::AuthFilter");
    ADD_METHOD_TO(AgentController::loginLogs,     "/api/agents/login-logs",     drogon::Get,  "aegis::AuthFilter");
    ADD_METHOD_TO(AgentController::adjustBalance,  "/api/agents/balance",        drogon::Post, "aegis::AuthFilter");
    ADD_METHOD_TO(AgentController::myBalance,      "/api/agents/my-balance",     drogon::Get,  "aegis::AuthFilter");
    METHOD_LIST_END

    drogon::Task<> list(drogon::HttpRequestPtr req,
                        std::function<void(const drogon::HttpResponsePtr&)> cb);
    drogon::Task<> subtree(drogon::HttpRequestPtr req,
                           std::function<void(const drogon::HttpResponsePtr&)> cb);
    drogon::Task<> create(drogon::HttpRequestPtr req,
                          std::function<void(const drogon::HttpResponsePtr&)> cb);
    drogon::Task<> update(drogon::HttpRequestPtr req,
                          std::function<void(const drogon::HttpResponsePtr&)> cb);
    drogon::Task<> setStatus(drogon::HttpRequestPtr req,
                             std::function<void(const drogon::HttpResponsePtr&)> cb);
    drogon::Task<> resetPassword(drogon::HttpRequestPtr req,
                                 std::function<void(const drogon::HttpResponsePtr&)> cb);
    drogon::Task<> loginLogs(drogon::HttpRequestPtr req,
                             std::function<void(const drogon::HttpResponsePtr&)> cb);
    drogon::Task<> adjustBalance(drogon::HttpRequestPtr req,
                                 std::function<void(const drogon::HttpResponsePtr&)> cb);
    drogon::Task<> myBalance(drogon::HttpRequestPtr req,
                             std::function<void(const drogon::HttpResponsePtr&)> cb);

    // 提价联动：把 rootId 的卡种价作为其整棵子树的价格下限，自上而下传播（只升不降）。
    // 返回被调整的下级数量；在调用方的事务内执行。
    static drogon::Task<int> cascadePriceFloor(
        std::shared_ptr<drogon::orm::Transaction> trx,
        const std::string& rootId, const std::string& rootTypesJson);
};

} // namespace aegis

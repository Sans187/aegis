#pragma once
#include <drogon/HttpController.h>

namespace aegis {

class StatsController : public drogon::HttpController<StatsController> {
public:
    METHOD_LIST_BEGIN
    ADD_METHOD_TO(StatsController::overview, "/api/stats", drogon::Get, "aegis::AuthFilter");
    ADD_METHOD_TO(StatsController::agentSales, "/api/stats/agent-sales", drogon::Get, "aegis::AuthFilter");
    METHOD_LIST_END

    drogon::Task<> overview(drogon::HttpRequestPtr, std::function<void(const drogon::HttpResponsePtr&)>);
    // 旗下代理某日销售明细（可选 ?date=YYYY-MM-DD，默认今天），含按卡种的销量拆分
    drogon::Task<> agentSales(drogon::HttpRequestPtr, std::function<void(const drogon::HttpResponsePtr&)>);
};

} // namespace aegis

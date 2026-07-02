#pragma once
// VerifyApp —— 单个应用的验证处理器（合并旧 BaseAPP + App1..App5）。
// 支持两种模式：card（卡密）/ user（账号密码）。密码一律 Argon2 哈希存储。

#include <drogon/drogon.h>
#include <drogon/orm/Row.h>
#include <trantor/net/TcpServer.h>
#include <json/json.h>
#include <string>
#include <unordered_set>

namespace aegis::tcp {

class VerifyApp {
public:
    VerifyApp(int appId, std::unordered_set<int> enabledApis);

    int appId() const { return appId_; }

    // v2 协议入口：按 apiId 跑业务逻辑并返回 Json 结果（加密/发送由 VerifyServiceV2 负责）。
    drogon::Task<Json::Value> apiJson(int apiId, const std::string& ip, Json::Value data);

private:
    drogon::Task<std::string> appMode();                 // "card" / "user"

    drogon::Task<Json::Value> doLogin(const std::string& ip, const Json::Value& data);
    drogon::Task<Json::Value> doLoginCard(const std::string& ip, const Json::Value& data);
    drogon::Task<Json::Value> doLoginAccount(const std::string& ip, const Json::Value& data);
    drogon::Task<Json::Value> doRegister(const std::string& ip, const Json::Value& data);
    drogon::Task<Json::Value> doRecharge(const Json::Value& data);   // 账号模式用卡续费
    // 凭据校验通过后的公共收尾（到期/冻结/换绑/发 token/置在线）
    drogon::Task<Json::Value> finishLogin(const drogon::orm::Row& row, const std::string& username,
                                          const std::string& hwid, const std::string& ip);

    drogon::Task<Json::Value> doRebind(const std::string& username, const std::string& hwid);
    drogon::Task<Json::Value> doUnbind(const std::string& username);
    drogon::Task<Json::Value> doQuery(const Json::Value& data);
    drogon::Task<Json::Value> doNotice();
    drogon::Task<Json::Value> doUpdateInfo();

    // 校验请求里的凭据（card: Key；user: username+password），通过返回用户名，否则 nullopt。
    drogon::Task<std::optional<std::string>> authUser(const Json::Value& data);

    int                     appId_;
    std::string             appIdStr_;
    std::unordered_set<int> apis_;
};

} // namespace aegis::tcp

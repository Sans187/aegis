#include "http/Permissions.h"
#include "security/Password.h"

using namespace drogon;

namespace aegis::perm {

drogon::Task<bool> isSuper(std::string agentId) {
    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro("SELECT level FROM agents WHERE id=$1 LIMIT 1;", agentId);
    co_return !r.empty() && r[0]["level"].as<int>() == 1;
}

drogon::Task<bool> checkSecurityPassword(std::string agentId, std::string password) {
    if (password.empty()) co_return false;
    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro(
        "SELECT security_password_hash FROM agents WHERE id=$1 LIMIT 1;", agentId);
    if (r.empty() || r[0]["security_password_hash"].isNull()) co_return false;
    co_return security::verifyPassword(password, r[0]["security_password_hash"].as<std::string>());
}

drogon::Task<std::vector<std::string>> subAgentIds(std::string agentId) {
    std::vector<std::string> out;
    auto db = app().getDbClient();
    // 递归 CTE：一次查询取整棵子树，替代旧版逐节点 DFS 的 N 次查询。
    auto r = co_await db->execSqlCoro(
        "WITH RECURSIVE sub AS ("
        "  SELECT id FROM agents WHERE id=$1 "
        "  UNION ALL "
        "  SELECT a.id FROM agents a JOIN sub ON a.parent_id = sub.id"
        ") SELECT id FROM sub;",
        agentId);
    out.reserve(r.size());
    for (const auto& row : r) out.push_back(row["id"].as<std::string>());
    if (out.empty()) out.push_back(agentId);  // 兜底：至少包含自己
    co_return out;
}

drogon::Task<int> agentLevel(std::string agentId) {
    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro("SELECT level FROM agents WHERE id=$1 LIMIT 1;", agentId);
    co_return r.empty() ? -1 : r[0]["level"].as<int>();
}

drogon::Task<bool> hasPermission(std::string agentId, std::string permKey) {
    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro("SELECT level, perms FROM agents WHERE id=$1 LIMIT 1;", agentId);
    if (r.empty()) co_return false;
    if (r[0]["level"].as<int>() == 1) co_return true;  // 超管放行

    const auto s = r[0]["perms"].as<std::string>();
    Json::Value perms;
    Json::CharReaderBuilder b;
    std::string errs;
    std::unique_ptr<Json::CharReader> rd(b.newCharReader());
    if (rd->parse(s.data(), s.data() + s.size(), &perms, &errs) && perms.isArray()) {
        for (const auto& p : perms)
            if (p.asString() == permKey) co_return true;
    }
    co_return false;
}

drogon::Task<bool> canAccessApp(std::string agentId, std::string appId) {
    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro("SELECT level, apps FROM agents WHERE id=$1 LIMIT 1;", agentId);
    if (r.empty()) co_return false;
    if (r[0]["level"].as<int>() == 1) co_return true;  // 超管全可

    // 拥有该 app？
    auto owns = co_await db->execSqlCoro(
        "SELECT 1 FROM apps WHERE id=$1 AND owner_id=$2 LIMIT 1;", appId, agentId);
    if (!owns.empty()) co_return true;

    // 在被分配的 apps 列表内？
    const auto s = r[0]["apps"].as<std::string>();
    Json::Value apps;
    Json::CharReaderBuilder b;
    std::string errs;
    std::unique_ptr<Json::CharReader> rd(b.newCharReader());
    if (rd->parse(s.data(), s.data() + s.size(), &apps, &errs) && apps.isArray()) {
        for (const auto& a : apps)
            if (a.asString() == appId) co_return true;
    }
    co_return false;
}

drogon::Task<bool> canManageApp(std::string agentId, std::string appId) {
    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro("SELECT level FROM agents WHERE id=$1 LIMIT 1;", agentId);
    if (r.empty()) co_return false;
    if (r[0]["level"].as<int>() == 1) co_return true;            // 超管
    auto owns = co_await db->execSqlCoro(
        "SELECT 1 FROM apps WHERE id=$1 AND owner_id=$2 LIMIT 1;", appId, agentId);
    co_return !owns.empty();                                     // 仅属主
}

drogon::Task<BalanceInfo> balanceInfo(std::string agentId) {
    BalanceInfo bi;
    auto db = app().getDbClient();
    auto r = co_await db->execSqlCoro(
        "SELECT a.level, a.balance, a.consumed, "
        "  COALESCE((SELECT SUM(balance) FROM agents c WHERE c.parent_id=a.id),0) AS children_alloc, "
        "  COALESCE((SELECT SUM(price)   FROM cards  k WHERE k.maker_id=a.id AND k.status='unused'),0) AS held "
        "FROM agents a WHERE a.id=$1 LIMIT 1;", agentId);
    if (r.empty()) co_return bi;
    bi.super         = r[0]["level"].as<int>() == 1;
    bi.balance       = r[0]["balance"].as<double>();
    bi.consumed      = r[0]["consumed"].as<double>();
    bi.childrenAlloc = r[0]["children_alloc"].as<double>();
    bi.held          = r[0]["held"].as<double>();
    co_return bi;
}

} // namespace aegis::perm
